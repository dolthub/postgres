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
 * The checks of pg_checkversioned over a stopped server's data directory,
 * changing nothing: every repository's packs, both generations of its
 * manifest, and every chunk both generations' base trees reach, read and
 * decoded as a read reads and decodes them, with the WAL ranges a fallback
 * would replay, which the program's C reads.
 */
#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/file.h"
#include "doltgres/chunks/header.h"
#include "doltgres/chunks/packs.h"
#include "doltgres/chunks/packs/check.h"
#include "doltgres/chunks/packs/open.h"
#include "doltgres/host/lib.h"
#include "doltgres/repo/lib.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/num.h"
#include "doltgres/std/path.h"
#include "doltgres/std/slice.h"
#include "doltgres/std/unwind.h"
#include "doltgres/std/vec.h"
#include "doltgres/store/fsck.h"
#include "doltgres/trees/node.h"
#include "doltgres/trees/published.h"

#ifdef DOLTGRES_TEST_STORE
#include "doltgres/chunks/address.h"
#include "doltgres/chunks/pack.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/shared.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/shared.h"
#include "doltgres/simc/simc.h"
#include "doltgres/std/io.h"
#include "doltgres/std/str.h"
#include "doltgres/std/test.h"
#include "doltgres/std/test_fs.h"
#include "doltgres/std/tuple.h"
#include "doltgres/store/fixtures.h"
#include "doltgres/store/open.h"
#endif

#define RUST_FILE "store" STD_SEP "src" STD_SEP "fsck.rs"

/* store_fsck_Lines_drop drops a Vec<String> of a check's lines. */
static void
store_fsck_Lines_drop(StdVec *lines)
{
	std_vec_drop_Checked(lines, sizeof(StdString), _Alignof(StdString), std_String_drop);
}

/* store_fsck_Fsck_drop drops what a check of a data directory found. */
void
store_fsck_Fsck_drop(StoreFsckFsck *self)
{
	store_fsck_Lines_drop(&self->found);
	store_fsck_Lines_drop(&self->settled);
	store_fsck_Lines_drop(&self->notes);
	std_vec_free_Checked(&self->kept, sizeof(StoreFsckKeptWal), _Alignof(StoreFsckKeptWal));
}

/* store_fsck_push formats a line and appends it to a check's lines. */
static void
store_fsck_push(StdVec *lines, const StdFmtArguments *arguments)
{
	StdString	line;

	std_fmt_format_Checked(&line, arguments);
	std_vec_push_Checked(lines, &line, sizeof(line), _Alignof(StdString));
}

/* store_fsck_push_text appends a line of text to a check's lines. */
static void
store_fsck_push_text(StdVec *lines, const char *text)
{
	StdString	line;

	std_string_from_Checked(&line, text, strlen(text));
	std_vec_push_Checked(lines, &line, sizeof(line), _Alignof(StdString));
}

/* store_fsck_finding returns chunks' error as a finding's text, which it takes. */
static StdString
store_fsck_finding(ChunksErrorError *error)
{
	StdString	finding;

	chunks_error_Error_into_finding(error, &finding);
	return finding;
}

/* store_fsck_named appends a repository's lines to a check's, each after the repository's name. */
static void
store_fsck_named(StdVec *lines, const StdString *name, const StdVec *own)
{
	for (size_t k = 0, k_len = own->len; k < k_len; k++)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1)};
		StdFmtArg	args[] = {{name, std_String_Display_fmt}, {&STD_VEC_AT(own, StdString, k), std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		store_fsck_push(lines, &arguments);
	}
}

/* store_fsck_Expected is Expected: what an internal tree node says of a child, the entries under it and its last key. */
typedef struct StoreFsckExpected
{
	uint64		entries;
	StdVec		last;
} StoreFsckExpected;

/* store_fsck_pending_t is a chunk a walk is to read: its address and locator, the node that names it, and what that node says of it. */
typedef struct store_fsck_pending_t
{
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
	bool		has_parent;
	ChunksAddressAddress parent;
	bool		has_expected;
	StoreFsckExpected expected;
} store_fsck_pending_t;

/* store_fsck_Pending_drop drops a walk's chunks to read, what each parent says of them. */
static void
store_fsck_Pending_drop(StdVec *pending)
{
	for (size_t k = 0, k_len = pending->len; k < k_len; k++)
		if (STD_VEC_AT(pending, store_fsck_pending_t, k).has_expected)
			std_vec_free_Checked(&STD_VEC_AT(pending, store_fsck_pending_t, k).expected.last, 1, 1);
	std_vec_free_Checked(pending, sizeof(store_fsck_pending_t), _Alignof(store_fsck_pending_t));
}

/* store_fsck_address_cmp is Address's Ord, for a BTreeSet<Address>. */
static StdOrdering
store_fsck_address_cmp(const void *a, const void *b)
{
	return chunks_address_Address_Ord_cmp(a, b);
}

/* store_fsck_walked_insert inserts an address into a BTreeSet<Address> and reports whether it was new. */
static bool
store_fsck_walked_insert(StdVec *walked, const ChunksAddressAddress *address)
{
	size_t		at;

	if (std_slice_binary_search(walked->ptr, walked->len, sizeof(*address), address, store_fsck_address_cmp, &at))
		return false;
	std_vec_insert(walked, at, address, sizeof(*address), _Alignof(ChunksAddressAddress), RUST_FILE, 0, 0);
	return true;
}

/* store_fsck_header_addresses counts the addresses a stored chunk's header names, for a chunk's split. */
static bool
store_fsck_header_addresses(void *captures, const uint8 *hashed, size_t hashed_len, size_t *result,
							ChunksErrorError *error)
{
	ChunksHeaderHeader header;
	size_t		read;

	(void) captures;
	if (!chunks_header_Header_read(hashed, hashed_len, &header, &read, error))
		return false;
	*result = chunks_header_Addresses_len(&header.addresses);
	chunks_header_Header_drop(&header);
	return true;
}

/* store_fsck_kind returns a chunk's kind, read from its header. */
static bool
store_fsck_kind(const ChunksChunkChunk *chunk, ChunksHeaderKind *result, ChunksErrorError *error)
{
	ChunksTrailerStored split;
	ChunksHeaderHeader header;
	size_t		read;

	if (!chunks_chunk_Chunk_split(chunk, store_fsck_header_addresses, NULL, NULL, &split, error) ||
		!chunks_header_Header_read(split.hashed.ptr, split.hashed.len, &header, &read, error))
		return false;
	*result = header.kind;
	chunks_header_Header_drop(&header);
	return true;
}

/* store_fsck_walk_frame_0_t is walk's state across its pad: the chunks to read, a chunk read, its flat node, and its parent's name. */
typedef struct store_fsck_walk_frame_0_t
{
	ChunksFileInterruptible io;
	ChunksPacksCheckChecked *checked;
	const StdString *name;
	ChunksAddressAddress root_address;
	ChunksTrailerLocator root_locator;
	StdVec	   *walked;
	StdVec	   *found;
	/* pending is a Vec of store_fsck_pending_t. */
	StdVec		pending;
	bool		pending_live;
	store_fsck_pending_t item;
	bool		item_live;
	StdString	named;
	bool		named_live;
	ChunksChunkChunk chunk;
	bool		chunk_live;
	TreesNodeFlat flat;
	bool		flat_live;
	/* addresses is a Vec<(u32, Locator)> of a leaf value's chunks. */
	StdVec		addresses;
	bool		addresses_live;
} store_fsck_walk_frame_0_t;

/* store_fsck_walk_pad_0 is walk's pad: a value's chunks, the flat node, the chunk, the parent's name, the item, then the chunks to read. */
static void
store_fsck_walk_pad_0(void *frame_)
{
	store_fsck_walk_frame_0_t *frame = frame_;

	if (frame->addresses_live)
	{
		frame->addresses_live = false;
		std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
	}
	if (frame->flat_live)
	{
		frame->flat_live = false;
		trees_node_Flat_drop(&frame->flat);
	}
	if (frame->chunk_live)
	{
		frame->chunk_live = false;
		chunks_chunk_Chunk_drop(&frame->chunk);
	}
	if (frame->named_live)
	{
		frame->named_live = false;
		std_String_drop(&frame->named);
	}
	if (frame->item_live)
	{
		frame->item_live = false;
		if (frame->item.has_expected)
			std_vec_free_Checked(&frame->item.expected.last, 1, 1);
	}
	if (frame->pending_live)
	{
		frame->pending_live = false;
		store_fsck_Pending_drop(&frame->pending);
	}
}

/* store_fsck_failed appends a finding of a chunk a node or the manifest names, with chunks' error, which it takes, after the text given. */
static void
store_fsck_failed(store_fsck_walk_frame_0_t *frame, const char *what, const char *after, ChunksErrorError *error)
{
	StdString	finding = store_fsck_finding(error);
	StdStr		kind = {what, strlen(what)};
	StdStr		then = {after, strlen(after)};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1), STD_FMT_LIT(" "), STD_FMT_ARG(2),
		STD_FMT_LIT(", which "), STD_FMT_ARG(3), STD_FMT_LIT(" names, "), STD_FMT_ARG(4), STD_FMT_LIT(": "),
	STD_FMT_ARG(5)};
	StdFmtArg	args[] = {{frame->name, std_String_Display_fmt}, {&kind, std_str_Display_fmt},
	{&frame->item.address, chunks_address_Address_Display_fmt}, {&frame->named, std_String_Display_fmt},
	{&then, std_str_Display_fmt}, {&finding, std_String_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	store_fsck_push(frame->found, &arguments);
	std_String_drop(&finding);
}

/* store_fsck_compared compares what a node holds with what its parent says of it, appending a finding when they differ. */
static void
store_fsck_compared(store_fsck_walk_frame_0_t *frame, const TreesNodeNode *node)
{
	size_t		len = trees_node_Node_len(node);
	StdBytes	last = {NULL, 0};
	uint64		total = trees_node_Node_total(node);
	const StoreFsckExpected *expected = &frame->item.expected;
	StdFmtSpec	hex = {.sign_aware_zero_pad = true, .debug_lower_hex = true, .fill = ' ',
	.align = STD_FMT_ALIGN_UNKNOWN, .has_width = true, .width = 2};

	if (len > 0)
		last = trees_node_Node_key(node, len - 1);
	if (total != expected->entries || len == 0 || last.len != expected->last.len ||
		memcmp(last.ptr, expected->last.ptr, last.len) != 0)
	{
		StdBytes	said = {expected->last.ptr, expected->last.len};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": node "), STD_FMT_ARG(1), STD_FMT_LIT(" holds "),
			STD_FMT_ARG(2), STD_FMT_LIT(" entries ending at key "), {NULL, 0, 3, hex}, STD_FMT_LIT(", where "),
			STD_FMT_ARG(4), STD_FMT_LIT(" says "), STD_FMT_ARG(5), STD_FMT_LIT(" ending at key "),
		{NULL, 0, 6, hex}};
		StdFmtArg	args[] = {{frame->name, std_String_Display_fmt},
		{&frame->item.address, chunks_address_Address_Display_fmt}, {&total, std_u64_Display_fmt},
		{&last, std_bytes_Debug_fmt}, {&frame->named, std_String_Display_fmt},
		{&expected->entries, std_u64_Display_fmt}, {&said, std_bytes_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		store_fsck_push(frame->found, &arguments);
	}
}

/* store_fsck_pushed appends a chunk to read to a walk's pending. */
static void
store_fsck_pushed(StdVec *pending, const store_fsck_pending_t *item)
{
	std_vec_push_Checked(pending, item, sizeof(*item), _Alignof(store_fsck_pending_t));
}

/* store_fsck_children appends what a node names to the chunks to read, its last entry's first so they pop in order. */
static void
store_fsck_children(store_fsck_walk_frame_0_t *frame, const TreesNodeNode *node)
{
	for (size_t i = trees_node_Node_len(node); i-- > 0;)
	{
		if (!trees_node_Node_is_leaf(node))
		{
			StdBytes	key = trees_node_Node_key(node, i);
			store_fsck_pending_t child = {.address = trees_node_Node_child(node, i),
				.locator = trees_node_Node_locator(node, i), .has_parent = true, .parent = frame->item.address,
			.has_expected = true, .expected = {.entries = trees_node_Node_subtree(node, i)}};

			std_slice_to_vec_Checked(&child.expected.last, key.ptr, key.len, 1, 1);
			store_fsck_pushed(&frame->pending, &child);
			continue;
		}
		{
			StdBytes	value = trees_node_Node_value(node, i);

			trees_node_Node_value_addresses(node, i, &frame->addresses);
			frame->addresses_live = true;
			for (size_t k = frame->addresses.len; k-- > 0;)
			{
				chunks_TupleU32Locator_t named = STD_VEC_AT(&frame->addresses, chunks_TupleU32Locator_t, k);
				size_t		at = (size_t) named.f0;
				size_t		end = STD_ADD(usize, at, 32, 255, 45);

				if (at <= end && end <= value.len)
				{
					store_fsck_pending_t chunk = {.locator = named.f1, .has_parent = true,
					.parent = frame->item.address};

					memcpy(&chunk.address, value.ptr + at, sizeof(chunk.address));
					store_fsck_pushed(&frame->pending, &chunk);
				}
				else
				{
					StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": node "), STD_FMT_ARG(1),
						STD_FMT_LIT("'s entry "), STD_FMT_ARG(2), STD_FMT_LIT(" names a chunk at offset "),
					STD_FMT_ARG(3), STD_FMT_LIT(" past its value's end")};
					StdFmtArg	args[] = {{frame->name, std_String_Display_fmt},
					{&frame->item.address, chunks_address_Address_Display_fmt}, {&i, std_usize_Display_fmt},
					{&at, std_usize_Display_fmt}};
					StdFmtArguments arguments = {pieces, lengthof(pieces), args};

					store_fsck_push(frame->found, &arguments);
				}
			}
			frame->addresses_live = false;
			std_vec_free_Checked(&frame->addresses, sizeof(chunks_TupleU32Locator_t), _Alignof(chunks_TupleU32Locator_t));
		}
	}
}

/* store_fsck_walk_body_0 is walk's body: each chunk to read, read and decoded once, compared with what its parent says, and what it names. */
static void
store_fsck_walk_body_0(void *frame_)
{
	store_fsck_walk_frame_0_t *frame = frame_;
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	store_fsck_pending_t root = {.address = frame->root_address, .locator = frame->root_locator};

	std_vec_with_capacity_Checked(&frame->pending, 1, sizeof(store_fsck_pending_t), _Alignof(store_fsck_pending_t));
	frame->pending_live = true;
	store_fsck_pushed(&frame->pending, &root);
	while (frame->pending.len != 0)
	{
		ChunksErrorError failed;
		ChunksHeaderKind kind;
		TreesNodeNode node;

		frame->item = STD_VEC_AT(&frame->pending, store_fsck_pending_t, frame->pending.len - 1);
		frame->pending.len--;
		frame->item_live = true;
		if (!store_fsck_walked_insert(frame->walked, &frame->item.address))
			goto next;
		if (frame->item.has_parent)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("node "), STD_FMT_ARG(0)};
			StdFmtArg	args[] = {{&frame->item.parent, chunks_address_Address_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			std_fmt_format_Checked(&frame->named, &arguments);
		}
		else
			std_string_from_Checked(&frame->named, "the manifest", strlen("the manifest"));
		frame->named_live = true;
		if (!chunks_packs_check_Checked_read(frame->checked, read, &frame->item.address, frame->item.locator,
											 &frame->chunk, &failed))
		{
			store_fsck_failed(frame, "chunk", "can't be read", &failed);
			goto named;
		}
		frame->chunk_live = true;
		if (!store_fsck_kind(&frame->chunk, &kind, &failed))
		{
			store_fsck_failed(frame, "chunk", "doesn't decode", &failed);
			goto chunk;
		}
		if (kind != CHUNKS_HEADER_TREE_LEAF && kind != CHUNKS_HEADER_TREE_INTERNAL)
			goto chunk;
		if (!trees_node_Flat_of(&frame->chunk, &frame->flat, &failed))
		{
			store_fsck_failed(frame, "node", "doesn't decode", &failed);
			goto chunk;
		}
		frame->flat_live = true;
		node = trees_node_Flat_node(&frame->flat);
		if (frame->item.has_expected)
			store_fsck_compared(frame, &node);
		store_fsck_children(frame, &node);
		frame->flat_live = false;
		trees_node_Flat_drop(&frame->flat);
chunk:
		frame->chunk_live = false;
		chunks_chunk_Chunk_drop(&frame->chunk);
named:
		frame->named_live = false;
		std_String_drop(&frame->named);
next:
		frame->item_live = false;
		if (frame->item.has_expected)
			std_vec_free_Checked(&frame->item.expected.last, 1, 1);
	}
	store_fsck_walk_pad_0(frame);
}

/*
 * store_fsck_walk reads every chunk a tree's root reaches, once each, in
 * order, as a read reads it: each tree node decoded, what its parent says of
 * it compared with what it holds, and each chunk its values name read in
 * turn.
 */
static void
store_fsck_walk(ChunksFileInterruptible io, ChunksPacksCheckChecked *checked, const StdString *name,
				ChunksAddressAddress address, ChunksTrailerLocator locator, StdVec *walked, StdVec *found)
{
	store_fsck_walk_frame_0_t frame = {.io = io, .checked = checked, .name = name, .root_address = address,
	.root_locator = locator, .walked = walked, .found = found};

	std_landing(&frame, store_fsck_walk_body_0, store_fsck_walk_pad_0);
}

/* store_fsck_read_t is a manifest generation's decode: its redo point and the roots of its base and identity trees, a Vec<(Address, Locator)>. */
typedef struct store_fsck_read_t
{
	bool		decoded;
	HostLsn		redo;
	StdVec		roots;
} store_fsck_read_t;

/* store_fsck_check_frame_0_t is check_repository's state across its pad: the repository's name and check, its manifest's generations and their decodes, and the chunks walked. */
typedef struct store_fsck_check_frame_0_t
{
	ChunksFileInterruptible io;
	StoreFsckFsck *fsck;
	uint32		repository;
	uint32		own;
	StdString	name;
	bool		name_live;
	ChunksPacksCheckChecked checked;
	bool		checked_live;
	StdPathBuf	path;
	bool		path_live;
	bool		has_newest;
	StdVec		newest;
	bool		has_previous;
	StdVec		previous;
	bool		has_newest_read;
	store_fsck_read_t newest_read;
	bool		has_previous_read;
	store_fsck_read_t previous_read;
	/* walked is a BTreeSet<Address>. */
	StdVec		walked;
	bool		walked_live;
} store_fsck_check_frame_0_t;

/* store_fsck_check_pad_0 is check_repository's pad: the chunks walked, the decodes, the generations, the path, the check, then the name. */
static void
store_fsck_check_pad_0(void *frame_)
{
	store_fsck_check_frame_0_t *frame = frame_;

	if (frame->walked_live)
	{
		frame->walked_live = false;
		std_vec_free_Checked(&frame->walked, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	}
	if (frame->has_previous_read)
	{
		frame->has_previous_read = false;
		if (frame->previous_read.decoded)
			std_vec_free_Checked(&frame->previous_read.roots, sizeof(chunks_TupleAddressLocator_t),
								 _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->has_newest_read)
	{
		frame->has_newest_read = false;
		if (frame->newest_read.decoded)
			std_vec_free_Checked(&frame->newest_read.roots, sizeof(chunks_TupleAddressLocator_t),
								 _Alignof(chunks_TupleAddressLocator_t));
	}
	if (frame->has_previous)
	{
		frame->has_previous = false;
		std_vec_free_Checked(&frame->previous, 1, 1);
	}
	if (frame->has_newest)
	{
		frame->has_newest = false;
		std_vec_free_Checked(&frame->newest, 1, 1);
	}
	if (frame->path_live)
	{
		frame->path_live = false;
		std_vec_free_Checked(&frame->path.bytes, 1, 1);
	}
	if (frame->checked_live)
	{
		frame->checked_live = false;
		chunks_packs_check_Checked_drop(&frame->checked);
	}
	if (frame->name_live)
	{
		frame->name_live = false;
		std_String_drop(&frame->name);
	}
}

/* store_fsck_said appends a repository's line of a text with a path in it and a finding after, which it takes when it has one. */
static void
store_fsck_said(StdVec *lines, const StdString *name, const char *before, const StdPathBuf *path, const char *after,
				StdString *finding)
{
	StdStr		empty = {"", 0};
	StdStr		head = {before, strlen(before)};
	StdStr		tail = {after, strlen(after)};
	StdStr		shown = {path == NULL ? "" : path->bytes.ptr, path == NULL ? 0 : path->bytes.len};
	StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": "), STD_FMT_ARG(1), STD_FMT_ARG(2), STD_FMT_ARG(3),
	STD_FMT_ARG(4)};
	StdFmtArg	args[] = {{name, std_String_Display_fmt}, {&head, std_str_Display_fmt}, {&shown, std_path_Display_fmt},
	{&tail, std_str_Display_fmt}, {finding == NULL ? (const void *) &empty : finding,
	finding == NULL ? std_str_Display_fmt : std_String_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	store_fsck_push(lines, &arguments);
	if (finding != NULL)
		std_String_drop(finding);
}

/* store_fsck_decoded decodes a manifest generation, appending a finding of one that doesn't decode. */
static void
store_fsck_decoded(store_fsck_check_frame_0_t *frame, const char *generation, const StdVec *bytes,
				   store_fsck_read_t *result)
{
	RepoManifest manifest;
	ChunksErrorError failed;

	result->decoded = false;
	if (!repo_Manifest_decode(bytes->ptr, bytes->len, trees_published_RepoId_unique(), &manifest, &failed))
	{
		StdString	finding = store_fsck_finding(&failed);
		StdStr		which = {generation, strlen(generation)};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": its "), STD_FMT_ARG(1),
		STD_FMT_LIT(" manifest doesn't decode: "), STD_FMT_ARG(2)};
		StdFmtArg	args[] = {{&frame->name, std_String_Display_fmt}, {&which, std_str_Display_fmt},
		{&finding, std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		store_fsck_push(&frame->fsck->found, &arguments);
		std_String_drop(&finding);
		return;
	}
	std_vec_new(&result->roots, sizeof(chunks_TupleAddressLocator_t), _Alignof(chunks_TupleAddressLocator_t));
	if (manifest.has_bases)
	{
		chunks_TupleAddressLocator_t root = {manifest.bases.address, manifest.bases.locator};

		std_vec_push_Checked(&result->roots, &root, sizeof(root), _Alignof(chunks_TupleAddressLocator_t));
	}
	if (manifest.has_identity)
	{
		chunks_TupleAddressLocator_t root = {manifest.identity.address, manifest.identity.locator};

		std_vec_push_Checked(&result->roots, &root, sizeof(root), _Alignof(chunks_TupleAddressLocator_t));
	}
	result->redo = manifest.redo;
	result->decoded = true;
	repo_Manifest_drop(&manifest);
}

/* store_fsck_kept appends a WAL range a repository's fallback would replay. */
static void
store_fsck_kept(StoreFsckFsck *fsck, uint32 repository, HostLsn from, bool oldest, bool found)
{
	StoreFsckKeptWal kept = {repository, from, oldest, found};

	std_vec_push_Checked(&fsck->kept, &kept, sizeof(kept), _Alignof(StoreFsckKeptWal));
}

/* store_fsck_manifests reads a repository's manifest's two generations, reporting through ended when the check ends with what it found. */
static void
store_fsck_manifests(store_fsck_check_frame_0_t *frame, bool *ended)
{
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	ChunksErrorError failed;
	ChunksFileFallback why;
	bool		has;
	bool		has_why;

	*ended = true;
	if (!chunks_file_read_kept(read, frame->path.bytes.ptr, frame->path.bytes.len, &has, &frame->newest, &has_why, &why,
							   &failed))
	{
		StdString	finding = store_fsck_finding(&failed);

		store_fsck_said(&frame->fsck->found, &frame->name, "its manifest can't be read: ", NULL, "", &finding);
		return;
	}
	if (!has)
	{
		store_fsck_said(&frame->fsck->found, &frame->name, "required file \"", &frame->path, "\" does not exist", NULL);
		return;
	}
	frame->has_newest = !has_why;
	if (has_why)
	{
		StdStr		shown = {frame->path.bytes.ptr, frame->path.bytes.len};
		StdFmtPiece pieces[] = {STD_FMT_ARG(0), STD_FMT_LIT(": its newest manifest \""), STD_FMT_ARG(1),
			STD_FMT_LIT("\" is not whole ("), STD_FMT_ARG(2),
		STD_FMT_LIT("), so the next start falls back to the previous")};
		StdFmtArg	args[] = {{&frame->name, std_String_Display_fmt}, {&shown, std_path_Display_fmt},
		{&why, chunks_file_Fallback_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_vec_free_Checked(&frame->newest, 1, 1);
		store_fsck_push(&frame->fsck->found, &arguments);
	}
	if (!chunks_file_read_previous(read, frame->path.bytes.ptr, frame->path.bytes.len, &frame->has_previous,
								   &frame->previous, &failed))
	{
		StdString	finding = store_fsck_finding(&failed);

		frame->has_previous = false;
		store_fsck_said(&frame->fsck->found, &frame->name, "its previous manifest can't be read: ", NULL, "",
						&finding);
	}
	*ended = false;
}

/* store_fsck_check_body_0 is check_repository's body: the packs' check, the manifest's generations, both generations' trees, and the WAL kept. */
static void
store_fsck_check_body_0(void *frame_)
{
	store_fsck_check_frame_0_t *frame = frame_;
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	StoreFsckFsck *fsck = frame->fsck;
	StdPathBuf	dir;
	bool		ended;
	bool		differ;
	bool		root_reads = false;

	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("database "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&frame->repository, std_u32_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&frame->name, &arguments);
	}
	frame->name_live = true;
	chunks_packs_check_check(frame->io, frame->repository, frame->own, &frame->checked);
	frame->checked_live = true;
	store_fsck_named(&fsck->found, &frame->name, &frame->checked.found);
	store_fsck_named(&fsck->settled, &frame->name, &frame->checked.settled);
	store_fsck_named(&fsck->notes, &frame->name, &frame->checked.notes);
	if (frame->checked.dropped)
	{
		store_fsck_said(&fsck->settled, &frame->name, "is dropped, whose directories the next start removes", NULL, "",
						NULL);
		goto done;
	}
	dir = host_tablespace_dir(frame->repository, frame->own);
	std_path_join(dir.bytes.ptr, dir.bytes.len, CHUNKS_PACKS_MANIFEST, strlen(CHUNKS_PACKS_MANIFEST), &frame->path);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	frame->path_live = true;
	store_fsck_manifests(frame, &ended);
	if (ended)
		goto done;
	if (frame->has_newest)
	{
		frame->has_newest_read = true;
		store_fsck_decoded(frame, "newest", &frame->newest, &frame->newest_read);
	}
	differ = frame->has_newest && (!frame->has_previous || frame->previous.len != frame->newest.len ||
								   memcmp(frame->previous.ptr, frame->newest.ptr, frame->newest.len) != 0);
	if ((differ || !frame->has_newest) && frame->has_previous)
	{
		frame->has_previous_read = true;
		store_fsck_decoded(frame, "previous", &frame->previous, &frame->previous_read);
	}
	std_vec_new(&frame->walked, sizeof(ChunksAddressAddress), _Alignof(ChunksAddressAddress));
	frame->walked_live = true;
	if (frame->has_newest_read && frame->newest_read.decoded)
	{
		root_reads = true;
		for (size_t k = 0, k_len = frame->newest_read.roots.len; k < k_len; k++)
		{
			chunks_TupleAddressLocator_t root = STD_VEC_AT(&frame->newest_read.roots, chunks_TupleAddressLocator_t, k);
			ChunksChunkChunk chunk;
			ChunksErrorError failed;
			bool		reads = chunks_packs_check_Checked_read(&frame->checked, read, &root.f0, root.f1, &chunk,
																&failed);

			if (reads)
				chunks_chunk_Chunk_drop(&chunk);
			else
			{
				StdString	finding = store_fsck_finding(&failed);

				std_String_drop(&finding);
			}
			root_reads = root_reads && reads;
			store_fsck_walk(frame->io, &frame->checked, &frame->name, root.f0, root.f1, &frame->walked, &fsck->found);
		}
	}
	if (frame->has_previous_read && frame->previous_read.decoded)
		for (size_t k = 0, k_len = frame->previous_read.roots.len; k < k_len; k++)
		{
			chunks_TupleAddressLocator_t root = STD_VEC_AT(&frame->previous_read.roots, chunks_TupleAddressLocator_t, k);

			store_fsck_walk(frame->io, &frame->checked, &frame->name, root.f0, root.f1, &frame->walked, &fsck->found);
		}
	if (differ)
	{
		store_fsck_said(&fsck->settled, &frame->name, "its manifest's two generations differ, which the next start evens",
						NULL, "", NULL);
		if (frame->has_newest_read && frame->newest_read.decoded)
			store_fsck_kept(fsck, frame->repository, frame->newest_read.redo, false, !root_reads);
	}
	if (!frame->has_newest && frame->has_previous_read && frame->previous_read.decoded)
		store_fsck_kept(fsck, frame->repository, frame->previous_read.redo, true, true);
done:
	store_fsck_check_pad_0(frame);
}

/* store_fsck_check_repository checks one repository's packs, its manifest's generations, and both generations' base trees. */
static void
store_fsck_check_repository(ChunksFileInterruptible io, StoreFsckFsck *fsck, uint32 repository, uint32 own)
{
	store_fsck_check_frame_0_t frame = {.io = io, .fsck = fsck, .repository = repository, .own = own};

	std_landing(&frame, store_fsck_check_body_0, store_fsck_check_pad_0);
}

/* store_fsck_fsck_frame_0_t is fsck's state across its pad: what it found, the repositories, and the tablespaces. */
typedef struct store_fsck_fsck_frame_0_t
{
	ChunksFileInterruptible io;
	StoreFsckFsck *result;
	bool		ok;
	/* repositories is a Vec<(NonZeroU32, u32)> of std_TupleU32U32_t. */
	StdVec		repositories;
	bool		repositories_live;
	/* spaces is a Vec<u32>. */
	StdVec		spaces;
	bool		spaces_live;
} store_fsck_fsck_frame_0_t;

/* store_fsck_fsck_pad_0 is fsck's pad: the tablespaces, the repositories, then what it found, on an unwind. */
static void
store_fsck_fsck_pad_0(void *frame_)
{
	store_fsck_fsck_frame_0_t *frame = frame_;

	if (frame->spaces_live)
	{
		frame->spaces_live = false;
		std_vec_free_Checked(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	}
	if (frame->repositories_live)
	{
		frame->repositories_live = false;
		std_vec_free_Checked(&frame->repositories, sizeof(std_TupleU32U32_t), _Alignof(std_TupleU32U32_t));
	}
	if (!frame->ok)
		store_fsck_Fsck_drop(frame->result);
}

/* store_fsck_located checks that each tablespace's location stands, noting one that is missing. */
static void
store_fsck_located(store_fsck_fsck_frame_0_t *frame)
{
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};

	for (size_t k = 0, k_len = frame->spaces.len; k < k_len; k++)
	{
		uint32		space = STD_VEC_AT(&frame->spaces, uint32, k);
		StdPathBuf	root = host_versioned_dir(space);
		StdStr		location = host_directory_of(root.bytes.ptr, root.bytes.len);
		ChunksErrorError failed;
		bool		there;

		if (!chunks_file_dir_there(read, location.ptr, location.len, &there, &failed))
		{
			StdString	finding = store_fsck_finding(&failed);
			StdFmtPiece pieces[] = {STD_FMT_LIT("could not read \""), STD_FMT_ARG(0), STD_FMT_LIT("\": "),
			STD_FMT_ARG(1)};
			StdFmtArg	args[] = {{&location, std_path_Display_fmt}, {&finding, std_String_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			store_fsck_push(&frame->result->found, &arguments);
			std_String_drop(&finding);
		}
		else if (!there)
		{
			StdFmtPiece pieces[] = {STD_FMT_LIT("tablespace "), STD_FMT_ARG(0), STD_FMT_LIT("'s location \""),
			STD_FMT_ARG(1), STD_FMT_LIT("\" is missing, so the versioned files in it are not checked")};
			StdFmtArg	args[] = {{&space, std_u32_Display_fmt}, {&location, std_path_Display_fmt}};
			StdFmtArguments arguments = {pieces, lengthof(pieces), args};

			store_fsck_push(&frame->result->notes, &arguments);
		}
		std_vec_free_Checked(&root.bytes, 1, 1);
	}
}

/* store_fsck_fsck_body_0 is fsck's body: the repositories and tablespaces listed, each tablespace's location, then each repository's check. */
static void
store_fsck_fsck_body_0(void *frame_)
{
	store_fsck_fsck_frame_0_t *frame = frame_;
	HostIoReadDoors read = {&frame->io.ops->read_doors, frame->io.self};
	StoreFsckFsck *fsck = frame->result;
	ChunksErrorError failed;
	HostError	error;

	frame->ok = false;
	std_vec_new(&fsck->found, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&fsck->settled, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&fsck->notes, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&fsck->kept, sizeof(StoreFsckKeptWal), _Alignof(StoreFsckKeptWal));
	if (!chunks_packs_open_repositories(read, &frame->repositories, &failed))
	{
		StdString	finding = store_fsck_finding(&failed);
		StdFmtPiece pieces[] = {STD_FMT_LIT("the repositories' directories can't be read: "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&finding, std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		store_fsck_push(&fsck->found, &arguments);
		std_String_drop(&finding);
		frame->ok = true;
		goto done;
	}
	frame->repositories_live = true;
	if (!host_tablespaces(read, &frame->spaces, &error))
	{
		ChunksErrorError host = chunks_error_Error_FromError_from(&error);
		StdString	finding = store_fsck_finding(&host);
		StdFmtPiece pieces[] = {STD_FMT_LIT("the tablespaces can't be listed: "), STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{&finding, std_String_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		store_fsck_push(&fsck->found, &arguments);
		std_String_drop(&finding);
		std_vec_new(&frame->spaces, sizeof(uint32), _Alignof(uint32));
	}
	frame->spaces_live = true;
	store_fsck_located(frame);
	if (frame->repositories.len != 0)
		store_fsck_push_text(&fsck->notes,
							 "which repositories no database names is left to the collector's report (Differences row 33)");
	for (size_t k = 0, k_len = frame->repositories.len; k < k_len; k++)
	{
		std_TupleU32U32_t entry = STD_VEC_AT(&frame->repositories, std_TupleU32U32_t, k);

		store_fsck_check_repository(frame->io, fsck, entry.f0, entry.f1);
	}
	frame->ok = true;
done:
	store_fsck_fsck_pad_0(frame);
}

/* store_fsck_fsck checks every repository the server's tablespaces hold, in order of id, noting each tablespace whose location is missing. */
void
store_fsck_fsck(ChunksFileInterruptible io, StoreFsckFsck *result)
{
	store_fsck_fsck_frame_0_t frame = {.io = io, .result = result};

	std_landing(&frame, store_fsck_fsck_body_0, store_fsck_fsck_pad_0);
}

#ifdef DOLTGRES_TEST_STORE

/* STORE_FSCK_TESTS_UNWRAP is Result::unwrap's panic text. */
#define STORE_FSCK_TESTS_UNWRAP "called `Result::unwrap()` on an `Err` value"

/* STORE_FSCK_TESTS_NOTE is the note every check of repositories makes. */
#define STORE_FSCK_TESTS_NOTE "which repositories no database names is left to the collector's report (Differences row 33)"

/* store_fsck_KeptWal_Debug_fmt is KeptWal's derived Debug. */
static bool
store_fsck_KeptWal_Debug_fmt(const void *value, StdFormatter *f)
{
	const StoreFsckKeptWal *kept = value;
	StdDebugStruct b = std_fmt_debug_struct(f, "KeptWal", 7);

	std_DebugStruct_field(&b, "repository", 10, &kept->repository, std_u32_Debug_fmt);
	std_DebugStruct_field(&b, "from", 4, &kept->from, std_u64_Debug_fmt);
	std_DebugStruct_field(&b, "oldest", 6, &kept->oldest, std_bool_Debug_fmt);
	std_DebugStruct_field(&b, "found", 5, &kept->found, std_bool_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* store_fsck_KeptWal_PartialEq_eq is KeptWal's derived PartialEq. */
static bool
store_fsck_KeptWal_PartialEq_eq(const StoreFsckKeptWal *self, const StoreFsckKeptWal *other)
{
	return self->repository == other->repository && self->from == other->from && self->oldest == other->oldest &&
		self->found == other->found;
}

/* store_fsck_tests_Kept_Debug_fmt is a Vec<KeptWal>'s Debug. */
static bool
store_fsck_tests_Kept_Debug_fmt(const void *value, StdFormatter *f)
{
	const StdVec *kept = value;
	StdDebugList l = std_fmt_debug_list(f);

	for (size_t k = 0, k_len = kept->len; k < k_len; k++)
		std_DebugList_entry(&l, &STD_VEC_AT(kept, StoreFsckKeptWal, k), store_fsck_KeptWal_Debug_fmt);
	return std_DebugList_finish(&l);
}

/* store_fsck_Fsck_Debug_fmt is Fsck's derived Debug. */
static bool
store_fsck_Fsck_Debug_fmt(const void *value, StdFormatter *f)
{
	const StoreFsckFsck *fsck = value;
	StdDebugStruct b = std_fmt_debug_struct(f, "Fsck", 4);

	std_DebugStruct_field(&b, "found", 5, &fsck->found, store_tests_Logs_Debug_fmt);
	std_DebugStruct_field(&b, "settled", 7, &fsck->settled, store_tests_Logs_Debug_fmt);
	std_DebugStruct_field(&b, "notes", 5, &fsck->notes, store_tests_Logs_Debug_fmt);
	std_DebugStruct_field(&b, "kept", 4, &fsck->kept, store_fsck_tests_Kept_Debug_fmt);
	return std_DebugStruct_finish(&b);
}

/* store_fsck_tests_lines_eq reports whether two Vec<String>s hold the same lines. */
static bool
store_fsck_tests_lines_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0, k_len = a->len; k < k_len; k++)
	{
		const StdString *x = &STD_VEC_AT(a, StdString, k);
		const StdString *y = &STD_VEC_AT(b, StdString, k);

		if (x->len != y->len || memcmp(x->ptr, y->ptr, x->len) != 0)
			return false;
	}
	return true;
}

/* store_fsck_tests_kept_eq reports whether two Vec<KeptWal>s hold the same ranges. */
static bool
store_fsck_tests_kept_eq(const StdVec *a, const StdVec *b)
{
	if (a->len != b->len)
		return false;
	for (size_t k = 0, k_len = a->len; k < k_len; k++)
		if (!store_fsck_KeptWal_PartialEq_eq(&STD_VEC_AT(a, StoreFsckKeptWal, k), &STD_VEC_AT(b, StoreFsckKeptWal, k)))
			return false;
	return true;
}

/* store_fsck_Fsck_PartialEq_eq is Fsck's derived PartialEq. */
static bool
store_fsck_Fsck_PartialEq_eq(const StoreFsckFsck *self, const StoreFsckFsck *other)
{
	return store_fsck_tests_lines_eq(&self->found, &other->found) &&
		store_fsck_tests_lines_eq(&self->settled, &other->settled) &&
		store_fsck_tests_lines_eq(&self->notes, &other->notes) && store_fsck_tests_kept_eq(&self->kept, &other->kept);
}

/* store_fsck_tests_checked runs a check of the test's data directory. */
static StoreFsckFsck
store_fsck_tests_checked(HostIoIo *io)
{
	ChunksFileInterruptible interruptible = {&CHUNKS_FILE_IO_INTERRUPTIBLE_OPS, io};
	StoreFsckFsck fsck;

	store_fsck_fsck(interruptible, &fsck);
	return fsck;
}

/* store_fsck_tests_lines returns a Vec<String> of lines given as text. */
static StdVec
store_fsck_tests_lines(const char *const *lines, size_t lines_len)
{
	StdVec		result;

	std_vec_new(&result, sizeof(StdString), _Alignof(StdString));
	for (size_t k = 0; k < lines_len; k++)
		store_fsck_push_text(&result, lines[k]);
	return result;
}

/* store_fsck_tests_empty returns an empty check's result, Fsck's Default. */
static StoreFsckFsck
store_fsck_tests_empty(void)
{
	StoreFsckFsck fsck;

	std_vec_new(&fsck.found, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&fsck.settled, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&fsck.notes, sizeof(StdString), _Alignof(StdString));
	std_vec_new(&fsck.kept, sizeof(StoreFsckKeptWal), _Alignof(StoreFsckKeptWal));
	return fsck;
}

/* store_fsck_tests_fsck_eq asserts at a line that a check found what is given, and drops both. */
static void
store_fsck_tests_fsck_eq(StoreFsckFsck *got, StoreFsckFsck *want, uint32 line)
{
	STD_ASSERT_EQ(line, 9, store_fsck_Fsck_PartialEq_eq(got, want), got, store_fsck_Fsck_Debug_fmt, want,
				  store_fsck_Fsck_Debug_fmt);
	store_fsck_Fsck_drop(got);
	store_fsck_Fsck_drop(want);
}

/* store_fsck_tests_lines_assert asserts at a line that a check's lines are the lines given as text. */
static void
store_fsck_tests_lines_assert(const StdVec *got, const char *const *lines, size_t lines_len, uint32 line)
{
	StdVec		want = store_fsck_tests_lines(lines, lines_len);

	STD_ASSERT_EQ(line, 9, store_fsck_tests_lines_eq(got, &want), got, store_tests_Logs_Debug_fmt, &want,
				  store_tests_Logs_Debug_fmt);
	store_fsck_Lines_drop(&want);
}

/* store_fsck_tests_kept_assert asserts at a line that a check's WAL ranges are the one given. */
static void
store_fsck_tests_kept_assert(const StdVec *got, uint32 repository, HostLsn from, bool oldest, bool found, uint32 line)
{
	StoreFsckKeptWal want = {repository, from, oldest, found};
	StdVec		wanted = {&want, 1, 1};

	STD_ASSERT_EQ(line, 9, store_fsck_tests_kept_eq(got, &wanted), got, store_fsck_tests_Kept_Debug_fmt, &wanted,
				  store_fsck_tests_Kept_Debug_fmt);
}

/* store_fsck_tests_any_found asserts at a line that a check found a line that starts with, or holds, a text, with the lines' Debug as the message. */
static void
store_fsck_tests_any_found(const StdVec *found, const char *text, size_t len, bool starts, uint32 line)
{
	bool		any = false;

	for (size_t k = 0, k_len = found->len; k < k_len && !any; k++)
	{
		const StdString *l = &STD_VEC_AT(found, StdString, k);

		any = starts ? l->len >= len && memcmp(l->ptr, text, len) == 0 :
			std_str_contains_str_Checked(l->ptr, l->len, text, len);
	}
	if (!any)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{found, store_tests_Logs_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, line, 9, &arguments);
	}
}

/* store_fsck_tests_one_found asserts at a line that a check found one line, which starts with a text, with the lines' Debug as the message. */
static void
store_fsck_tests_one_found(const StdVec *found, const char *text, size_t text_len, uint32 line)
{
	const StdString *l = found->len == 1 ? &STD_VEC_AT(found, StdString, 0) : NULL;

	if (l == NULL || l->len < text_len || memcmp(l->ptr, text, text_len) != 0)
	{
		StdFmtPiece pieces[] = {STD_FMT_ARG(0)};
		StdFmtArg	args[] = {{found, store_tests_Logs_Debug_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_panic_fmt(RUST_FILE, line, 9, &arguments);
	}
}

/* store_fsck_tests_ok asserts a call's success, which unwraps at a line and column. */
static void
store_fsck_tests_ok(bool ok, const HostError *error, uint32 line, uint32 col)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, col, STORE_FSCK_TESTS_UNWRAP, error, host_Error_Debug_fmt);
}

/* store_fsck_tests_chunks_ok asserts a call of chunks' success, which unwraps at a line and column. */
static void
store_fsck_tests_chunks_ok(bool ok, const ChunksErrorError *error, uint32 line, uint32 col)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, col, STORE_FSCK_TESTS_UNWRAP, error, chunks_error_Error_Debug_fmt);
}

/* store_fsck_tests_io_ok asserts a file call's success, which unwraps at a line and column. */
static void
store_fsck_tests_io_ok(bool ok, const StdIoError *error, uint32 line, uint32 col)
{
	if (!ok)
		STD_RESULT_EXPECT_FAILED(line, col, STORE_FSCK_TESTS_UNWRAP, error, std_io_Error_Debug_fmt);
}

/* store_fsck_tests_started runs the startup process's open at redo point 50, with no label, which unwraps at a line and column 40. */
static void
store_fsck_tests_started(HostIoIo *io, uint32 line)
{
	HostError	error;
	StdVec		warnings;
	StdVec		logs;

	store_fsck_tests_ok(store_open_startup_open(io, 50, 0, &error), &error, line, 40);
	warnings = simc_take_warnings();
	logs = simc_take_logs();
	store_tests_Warnings_drop(&warnings);
	store_tests_Logs_drop(&logs);
}

/* store_fsck_tests_evened makes a repository and starts once, which writes its newest manifest again, as a clean stop after a start leaves it. */
static void
store_fsck_tests_evened(HostIoIo *io, uint32 number)
{
	store_fixtures_made(io, number);
	store_fsck_tests_started(io, 282);
}

/* store_fsck_tests_newest returns a repository's newest manifest, decoded, whose read, option, and decode unwrap at lines and columns. */
static RepoManifest
store_fsck_tests_newest(HostIoIo *io, const StdPathBuf *path, uint32 line, uint32 col, uint32 some_col,
						uint32 decode_line, uint32 decode_col, StdVec *bytes)
{
	HostIoReadDoors read = HOST_IO_IO_READ_DOORS(io);
	ChunksErrorError failed;
	ChunksFileFallback why;
	RepoManifest manifest;
	bool		has;
	bool		has_why;

	store_fsck_tests_chunks_ok(chunks_file_read_kept(read, path->bytes.ptr, path->bytes.len, &has, bytes, &has_why, &why,
													 &failed), &failed, line, col);
	if (!has)
		std_option_unwrap_failed(RUST_FILE, line, some_col);
	store_fsck_tests_chunks_ok(repo_Manifest_decode(bytes->ptr, bytes->len, trees_published_RepoId_unique(), &manifest,
													&failed), &failed, decode_line, decode_col);
	return manifest;
}

/* store_fsck_tests_root returns the locator of the root of a repository's newest manifest's base tree, or its identity tree's. */
static ChunksTrailerLocator
store_fsck_tests_root(HostIoIo *io, uint32 number, bool identity)
{
	StdPathBuf	path = store_fixtures_manifest(number);
	StdVec		bytes;
	uint32		line = identity ? 294 : 288;
	RepoManifest manifest = store_fsck_tests_newest(io, &path, line, 55, 64, line + 1, 53, &bytes);
	ChunksTrailerLocator locator;

	if (!(identity ? manifest.has_identity : manifest.has_bases))
		std_option_unwrap_failed(RUST_FILE, line + 1, identity ? 98 : 95);
	locator = identity ? manifest.identity.locator : manifest.bases.locator;
	repo_Manifest_drop(&manifest);
	std_vec_free_Checked(&bytes, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	return locator;
}

/* store_fsck_tests_damage flips a byte in the middle of the record a locator names, in its repository's packs. */
static void
store_fsck_tests_damage(uint32 number, ChunksTrailerLocator locator)
{
	StdPathBuf	dir = host_tablespace_dir(number, HOST_PG_DEFAULT);
	StdVec		names;
	StdIoError	io_error;
	bool		entry;

	if (!std_fs_read_dir_names(dir.bytes.ptr, dir.bytes.len, &names, &entry, &io_error))
		store_fsck_tests_io_ok(false, &io_error, entry ? 302 : 301, entry ? 30 : 46);
	for (size_t k = 0, k_len = names.len; k < k_len; k++)
	{
		const StdString *name = &STD_VEC_AT(&names, StdString, k);
		StdPathBuf	path;
		StdVec		bytes;
		size_t		at;

		if (name->len <= 5 || memcmp((const char *) name->ptr + name->len - 5, ".pack", 5) != 0)
			continue;
		std_path_join(dir.bytes.ptr, dir.bytes.len, name->ptr, name->len, &path);
		store_fsck_tests_io_ok(std_fs_read(path.bytes.ptr, path.bytes.len, &bytes, &io_error), &io_error, 304, 54);
		at = STD_ADD(usize, std_usize_try_from_u64(chunks_trailer_Locator_offset(&locator)),
					 std_usize_try_from_u64(chunks_trailer_Locator_length(&locator) / 2), 305, 26);
		if (at < bytes.len)
		{
			STD_VEC_AT(&bytes, uint8, at) ^= 0xff;
			store_fsck_tests_io_ok(std_fs_write(path.bytes.ptr, path.bytes.len, bytes.ptr, bytes.len, &io_error),
								   &io_error, 308, 50);
		}
		std_vec_free_Checked(&bytes, 1, 1);
		std_vec_free_Checked(&path.bytes, 1, 1);
	}
	store_fsck_Lines_drop(&names);
	std_vec_free_Checked(&dir.bytes, 1, 1);
}

/* test_store_fsck_tests_no_repository_finds_nothing: a data directory with no repository finds and notes nothing. */
void
test_store_fsck_tests_no_repository_finds_nothing(void)
{
	SimcTestIo	test_io = simc_io();
	StoreFsckFsck got = store_fsck_tests_checked(&test_io.io);
	StoreFsckFsck want = store_fsck_tests_empty();

	store_fsck_tests_fsck_eq(&got, &want, 318);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_fsck_tests_an_evened_repository_finds_nothing: a repository a
 * start evened finds nothing, with the note that leaves the repositories no
 * database names to the collector.
 */
void
test_store_fsck_tests_an_evened_repository_finds_nothing(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StoreFsckFsck got;
	StoreFsckFsck want = store_fsck_tests_empty();

	store_fsck_tests_evened(io, 931);
	got = store_fsck_tests_checked(io);
	store_fsck_push_text(&want.notes, STORE_FSCK_TESTS_NOTE);
	store_fsck_tests_fsck_eq(&got, &want, 328);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_fsck_tests_differing_generations_are_settled_with_the_newests_range:
 * a manifest whose generations differ is what the next start evens, and the
 * WAL from the newest's redo point is a fallback's range, whose gap is
 * settled while the newest's base tree reads.
 */
void
test_store_fsck_tests_differing_generations_are_settled_with_the_newests_range(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StoreFsckFsck fsck;
	static const char *const settled[] = {"database 932: its manifest's two generations differ, which the next start evens"};

	store_fixtures_made(io, 932);
	fsck = store_fsck_tests_checked(io);
	store_fsck_tests_lines_assert(&fsck.found, NULL, 0, 339);
	store_fsck_tests_lines_assert(&fsck.settled, settled, lengthof(settled), 340);
	store_fsck_tests_kept_assert(&fsck.kept, 932, 20, false, false, 341);
	store_fsck_Fsck_drop(&fsck);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_fsck_tests_a_newest_root_that_cant_be_read_is_found_with_its_range:
 * a newest base tree whose root can't be read is found, and its fallback's
 * range is one whose gap is found.
 */
void
test_store_fsck_tests_a_newest_root_that_cant_be_read_is_found_with_its_range(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StoreFsckFsck fsck;

	store_fixtures_made(io, 933);
	store_fsck_tests_damage(933, store_fsck_tests_root(io, 933, false));
	fsck = store_fsck_tests_checked(io);
	store_fsck_tests_any_found(&fsck.found, "database 933: chunk ", strlen("database 933: chunk "), true, 352);
	store_fsck_tests_kept_assert(&fsck.kept, 933, 20, false, true, 353);
	store_fsck_Fsck_drop(&fsck);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_fsck_tests_a_newest_manifest_not_whole_is_found_with_the_previouss_range:
 * a newest manifest that isn't whole is found, and its fallback's range runs
 * from the previous's own redo point, starting at the oldest WAL held when
 * that isn't kept.
 */
void
test_store_fsck_tests_a_newest_manifest_not_whole_is_found_with_the_previouss_range(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	path;
	StdIoError	io_error;
	StdVec		bytes;
	StoreFsckFsck fsck;
	static const char found[] = "database 934: its newest manifest ";

	store_fixtures_made(io, 934);
	path = store_fixtures_manifest(934);
	store_fsck_tests_io_ok(std_fs_read(path.bytes.ptr, path.bytes.len, &bytes, &io_error), &io_error, 364, 46);
	std_index_check(0, bytes.len, RUST_FILE, 365, 9);
	STD_VEC_AT(&bytes, uint8, 0) ^= 0xff;
	store_fsck_tests_io_ok(std_fs_write(path.bytes.ptr, path.bytes.len, bytes.ptr, bytes.len, &io_error), &io_error,
						   366, 38);
	std_vec_free_Checked(&bytes, 1, 1);
	fsck = store_fsck_tests_checked(io);
	store_fsck_tests_one_found(&fsck.found, found, strlen(found), 368);
	store_fsck_tests_kept_assert(&fsck.kept, 934, 10, true, true, 373);
	store_fsck_Fsck_drop(&fsck);
	std_vec_free_Checked(&path.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* test_store_fsck_tests_a_dropped_repository_is_settled: a dropped repository is what the next start removes, and nothing else of it is checked. */
void
test_store_fsck_tests_a_dropped_repository_is_settled(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	dir;
	StdPathBuf	mark;
	StdIoError	io_error;
	StoreFsckFsck fsck;
	static const char *const settled[] = {"database 935: is dropped, whose directories the next start removes"};

	store_fsck_tests_evened(io, 935);
	dir = host_tablespace_dir(935, HOST_PG_DEFAULT);
	std_path_join(dir.bytes.ptr, dir.bytes.len, "dropped", 7, &mark);
	store_fsck_tests_io_ok(std_fs_write(mark.bytes.ptr, mark.bytes.len, (const uint8 *) "", 0, &io_error), &io_error,
						   382, 90);
	fsck = store_fsck_tests_checked(io);
	store_fsck_tests_lines_assert(&fsck.settled, settled, lengthof(settled), 384);
	if (fsck.found.len != 0 || fsck.kept.len != 0)
	{
		StdVec		none = {0};
		StdTupleRef left = {2, {STD_ITEM(&fsck.found, store_tests_Logs_Debug_fmt),
		STD_ITEM(&fsck.kept, store_fsck_tests_Kept_Debug_fmt)}};
		StdTupleRef right = {2, {STD_ITEM(&none, store_tests_Logs_Debug_fmt),
		STD_ITEM(&none, store_fsck_tests_Kept_Debug_fmt)}};

		std_assert_failed(RUST_FILE, 385, 9, "==", &left, std_tuple_Debug_fmt, &right, std_tuple_Debug_fmt, NULL);
	}
	store_fsck_Fsck_drop(&fsck);
	std_vec_free_Checked(&mark.bytes, 1, 1);
	std_vec_free_Checked(&dir.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/* store_fsck_tests_saving_t is a base save's lines and the base record's bytes, which name a chunk no pack holds. */
typedef struct store_fsck_tests_saving_t
{
	const StdVec *bytes;
	uint32		create;
	uint32		read;
	uint32		save;
	uint32		close;
	uint32		claim;
} store_fsck_tests_saving_t;

/* store_fsck_tests_base is a save's gathering: one storage's base, whose record names a chunk no pack holds. */
static bool
store_fsck_tests_base(void *captures, HostIoIo *io, const RepoRead *read, StdVec *result, ChunksErrorError *error)
{
	const store_fsck_tests_saving_t *saving = captures;
	RepoGathered gathered = {.tag = REPO_GATHERED_BASE};
	chunks_TupleU32Locator_t named = {0, chunks_trailer_Locator_at(1, 4096, 64)};

	(void) io;
	(void) read;
	(void) error;
	std_vec_with_capacity_Checked(result, 1, sizeof(RepoGathered), _Alignof(RepoGathered));
	gathered.u.base.storage.tablespace = HOST_PG_DEFAULT;
	gathered.u.base.storage.relation = 1;
	std_vec_clone(&gathered.u.base.record.bytes, saving->bytes, 1, 1);
	std_vec_with_capacity_Checked(&gathered.u.base.record.addresses, 1, sizeof(named), _Alignof(chunks_TupleU32Locator_t));
	std_vec_push_Checked(&gathered.u.base.record.addresses, &named, sizeof(named), _Alignof(chunks_TupleU32Locator_t));
	gathered.u.base.folded = 5;
	std_vec_push_Checked(result, &gathered, sizeof(gathered), _Alignof(RepoGathered));
	return true;
}

/* store_fsck_tests_saved is a save's after-write, which does nothing. */
static bool
store_fsck_tests_saved(void *captures, HostIoIo *io, ChunksErrorError *error)
{
	(void) captures;
	(void) io;
	(void) error;
	return true;
}

/* store_fsck_tests_saving makes a repository whose one base names a chunk no pack holds, with the chunk's address made of one byte, frees its record, and hands back the reader it read with. */
static ChunksAddressAddress
store_fsck_tests_saving(HostIoIo *io, uint32 number, uint8 byte, const store_fsck_tests_saving_t *lines,
						bool checkpointed, HostSharedReader *reading)
{
	ChunksAddressAddress missing;
	store_fsck_tests_saving_t saving = *lines;
	RepoRepo	repo;
	HostSharedOwned owned;
	ChunksErrorError failed;
	HostError	error;
	StdVec		bytes;
	uint64		location;
	bool		has;

	memset(&missing, byte, sizeof(missing));
	if (checkpointed)
		simc_checkpointed(10);
	store_fsck_tests_chunks_ok(repo_Repo_create(io, number, HOST_PG_DEFAULT, &repo, &failed), &failed, lines->create, 92);
	if (checkpointed)
		simc_checkpointed(20);
	store_fsck_tests_ok(host_shared_read(io, reading, &error), &error, lines->read, 46);
	std_slice_to_vec_Checked(&bytes, (const uint8 *) &missing, sizeof(missing), 1, 1);
	std_vec_extend_from_slice_Checked(&bytes, "base", 4, 1, 1);
	saving.bytes = &bytes;
	store_fsck_tests_chunks_ok(repo_Repo_save(&repo, io, reading, false, NULL, store_fsck_tests_base, &saving,
											  store_fsck_tests_saved, NULL, &failed), &failed, lines->save, 66);
	location = chunks_packs_backend_Packs_location(&repo.packs);
	store_fsck_tests_chunks_ok(repo_Repo_close(&repo, io, reading, &failed), &failed, lines->close, 34);
	store_fsck_tests_ok(host_shared_claim(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location,
										  &has, &owned, &error), &error, lines->claim, 83);
	if (!has)
		std_option_unwrap_failed(RUST_FILE, lines->claim, 92);
	host_shared_free(&owned);
	std_vec_free_Checked(&bytes, 1, 1);
	return missing;
}

/*
 * test_store_fsck_tests_a_chunk_a_base_record_names_that_no_pack_holds_is_found:
 * a chunk a base record names that no pack holds is found, named by the leaf
 * that names it.
 */
void
test_store_fsck_tests_a_chunk_a_base_record_names_that_no_pack_holds_is_found(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	store_fsck_tests_saving_t lines = {.create = 393, .read = 394, .save = 403, .close = 405, .claim = 406};
	HostSharedReader reading;
	ChunksAddressAddress missing = store_fsck_tests_saving(io, 936, 7, &lines, false, &reading);
	StoreFsckFsck fsck;
	StdString	found;
	StdFmtPiece pieces[] = {STD_FMT_LIT("database 936: chunk "), STD_FMT_ARG(0), STD_FMT_LIT(", which node ")};
	StdFmtArg	args[] = {{&missing, chunks_address_Address_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	store_fsck_tests_started(io, 407);
	fsck = store_fsck_tests_checked(io);
	std_fmt_format_Checked(&found, &arguments);
	store_fsck_tests_one_found(&fsck.found, found.ptr, found.len, 410);
	std_String_drop(&found);
	store_fsck_Fsck_drop(&fsck);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/* store_fsck_tests_written writes a pack of one chunk into a repository and lists it, returning its address and its locator. */
static chunks_TupleAddressLocator_t
store_fsck_tests_written(HostIoIo *io, const RepoRepo *repo, const HostSharedReader *reading, StdVec *hashed,
						 const ChunksTrailerLocator *locators, size_t locators_len)
{
	chunks_TupleAddressLocator_t written = {chunks_address_Address_of(hashed->ptr, hashed->len)};
	ChunksTrailerStamps stamps = chunks_trailer_Stamps_Default_default();
	ChunksPackPackWriterInDir writer;
	ChunksErrorError failed;
	ChunksPackPack pack;
	StdVec		stored;

	store_fsck_tests_chunks_ok(chunks_packs_backend_Packs_writer(&repo->packs, io, reading, 0, &writer, &failed),
							   &failed, 427, 65);
	chunks_trailer_store(hashed->ptr, hashed->len, locators, locators_len, &stamps, &stored);
	chunks_trailer_Stamps_drop(&stamps);
	store_fsck_tests_chunks_ok(chunks_pack_PackWriterInDir_write(&writer, io, written.f0, stored.ptr, stored.len, false,
																 &written.f1, &failed), &failed, 429, 25);
	std_vec_free_Checked(&stored, 1, 1);
	store_fsck_tests_chunks_ok(chunks_pack_PackWriterInDir_finish(&writer, HOST_IO_IO_TAKES_SHORT(io), &pack, &failed),
							   &failed, 430, 42);
	store_fsck_tests_chunks_ok(chunks_packs_backend_Packs_add(&repo->packs, io, reading, &pack, &failed), &failed, 431,
							   48);
	std_vec_free_Checked(hashed, 1, 1);
	return written;
}

/* store_fsck_tests_leaf returns a leaf of one key, whose value is "value" and names nothing. */
static StdVec
store_fsck_tests_leaf(const char *key)
{
	StdBytes	keys[] = {{(const uint8 *) key, strlen(key)}};
	StdBytes	values[] = {{(const uint8 *) "value", 5}};
	StdVec		addresses[1];
	StdVec		leaf;

	std_vec_new(&addresses[0], sizeof(uint32), _Alignof(uint32));
	trees_node_encode_leaf(keys, lengthof(keys), values, lengthof(values), addresses, lengthof(addresses), false, &leaf);
	return leaf;
}

/* store_fsck_tests_swap returns bytes with the first copy of some bytes in them replaced, which must be there. */
static StdVec
store_fsck_tests_swap(const StdVec *bytes, const uint8 *from, size_t from_len, const uint8 *to, size_t to_len)
{
	StdVec		result;
	size_t		at = 0;

	while (at + from_len <= bytes->len && memcmp((const uint8 *) bytes->ptr + at, from, from_len) != 0)
		at++;
	if (at + from_len > bytes->len)
		std_option_unwrap_failed(RUST_FILE, 447, 82);
	std_slice_to_vec_Checked(&result, bytes->ptr, at, 1, 1);
	std_vec_extend_from_slice_Checked(&result, to, to_len, 1, 1);
	std_vec_extend_from_slice_Checked(&result, (const uint8 *) bytes->ptr + at + from_len, bytes->len - at - from_len, 1,
									  1);
	return result;
}

/*
 * test_store_fsck_tests_a_node_whose_parent_says_another_count_is_found_naming_the_parent:
 * a node whose parent says another count of entries under it than it holds is
 * found, naming both.
 */
void
test_store_fsck_tests_a_node_whose_parent_says_another_count_is_found_naming_the_parent(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	RepoRepo	repo;
	HostSharedReader reading;
	HostSharedOwned owned;
	ChunksErrorError failed;
	HostError	error;
	chunks_TupleAddressLocator_t first;
	chunks_TupleAddressLocator_t second;
	chunks_TupleAddressLocator_t root;
	StdVec		hashed;
	StdPathBuf	path;
	StdVec		newest;
	RepoManifest manifest;
	StdVec		old_locator;
	StdVec		new_locator;
	StdVec		swapped;
	StdVec		crafted;
	StoreFsckFsck fsck;
	StdString	differs;
	uint64		location;
	bool		has;
	bool		written;

	store_fixtures_made(io, 937);
	store_fsck_tests_chunks_ok(repo_Repo_open(io, 937, HOST_PG_DEFAULT, &repo, &failed), &failed, 423, 90);
	store_fsck_tests_ok(host_shared_read(io, &reading, &error), &error, 424, 46);
	hashed = store_fsck_tests_leaf("a");
	first = store_fsck_tests_written(io, &repo, &reading, &hashed, NULL, 0);
	hashed = store_fsck_tests_leaf("b");
	second = store_fsck_tests_written(io, &repo, &reading, &hashed, NULL, 0);
	{
		StdBytes	keys[] = {{(const uint8 *) "a", 1}, {(const uint8 *) "b", 1}};
		ChunksAddressAddress children[] = {first.f0, second.f0};
		ChunksTrailerLocator locators[] = {first.f1, second.f1};
		static const uint64 counts[] = {1, 3};

		trees_node_encode_internal(1, keys, lengthof(keys), children, lengthof(children), counts, lengthof(counts),
								   &hashed);
		root = store_fsck_tests_written(io, &repo, &reading, &hashed, locators, lengthof(locators));
	}
	path = store_fixtures_manifest(937);
	manifest = store_fsck_tests_newest(io, &path, 440, 43, 52, 442, 57, &newest);
	if (!manifest.has_bases)
		std_option_unwrap_failed(RUST_FILE, 442, 99);
	std_vec_new(&old_locator, 1, 1);
	std_vec_new(&new_locator, 1, 1);
	chunks_trailer_Locator_write(&manifest.bases.locator, &old_locator);
	chunks_trailer_Locator_write(&root.f1, &new_locator);
	swapped = store_fsck_tests_swap(&newest, (const uint8 *) &manifest.bases.address, sizeof(ChunksAddressAddress),
									(const uint8 *) &root.f0, sizeof(ChunksAddressAddress));
	crafted = store_fsck_tests_swap(&swapped, old_locator.ptr, old_locator.len, new_locator.ptr, new_locator.len);
	store_fsck_tests_chunks_ok(chunks_file_write_kept(HOST_IO_IO_IO_DOORS(io), path.bytes.ptr, path.bytes.len, crafted.ptr,
													  crafted.len, &written, &failed), &failed, 451, 55);
	location = chunks_packs_backend_Packs_location(&repo.packs);
	store_fsck_tests_chunks_ok(repo_Repo_close(&repo, io, &reading, &failed), &failed, 453, 34);
	store_fsck_tests_ok(host_shared_claim(HOST_SHARED_IO_WITHIN(io), &CHUNKS_PACKS_SHARED_PACK_STATE_POOL_OPS, location,
										  &has, &owned, &error), &error, 454, 83);
	if (!has)
		std_option_unwrap_failed(RUST_FILE, 454, 92);
	host_shared_free(&owned);
	fsck = store_fsck_tests_checked(io);
	{
		StdFmtPiece pieces[] = {STD_FMT_LIT("database 937: node "), STD_FMT_ARG(0),
			STD_FMT_LIT(" holds 1 entries ending at key [62], where node "), STD_FMT_ARG(1),
		STD_FMT_LIT(" says 3 ending at key [62]")};
		StdFmtArg	args[] = {{&second.f0, chunks_address_Address_Display_fmt},
		{&root.f0, chunks_address_Address_Display_fmt}};
		StdFmtArguments arguments = {pieces, lengthof(pieces), args};

		std_fmt_format_Checked(&differs, &arguments);
	}
	store_fsck_tests_any_found(&fsck.found, differs.ptr, differs.len, false, 459);
	std_String_drop(&differs);
	store_fsck_Fsck_drop(&fsck);
	std_vec_free_Checked(&crafted, 1, 1);
	std_vec_free_Checked(&swapped, 1, 1);
	std_vec_free_Checked(&new_locator, 1, 1);
	std_vec_free_Checked(&old_locator, 1, 1);
	repo_Manifest_drop(&manifest);
	std_vec_free_Checked(&newest, 1, 1);
	std_vec_free_Checked(&path.bytes, 1, 1);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_fsck_tests_a_newest_whose_root_reads_settles_its_ranges_gap_though_a_deeper_chunk_is_missing:
 * a newest base tree whose root reads but whose deeper chunk is missing is
 * found, and its range's gap is what the next start settles, since the start
 * falls back only when the root can't be read.
 */
void
test_store_fsck_tests_a_newest_whose_root_reads_settles_its_ranges_gap_though_a_deeper_chunk_is_missing(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	store_fsck_tests_saving_t lines = {.create = 469, .read = 471, .save = 480, .close = 482, .claim = 483};
	HostSharedReader reading;
	ChunksAddressAddress missing = store_fsck_tests_saving(io, 938, 9, &lines, true, &reading);
	StoreFsckFsck fsck = store_fsck_tests_checked(io);
	StdString	found;
	StdFmtPiece pieces[] = {STD_FMT_LIT("chunk "), STD_FMT_ARG(0)};
	StdFmtArg	args[] = {{&missing, chunks_address_Address_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_fmt_format_Checked(&found, &arguments);
	store_fsck_tests_any_found(&fsck.found, found.ptr, found.len, false, 485);
	std_String_drop(&found);
	store_fsck_tests_kept_assert(&fsck.kept, 938, 20, false, false, 486);
	store_fsck_Fsck_drop(&fsck);
	host_shared_Reader_drop(&reading);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_fsck_tests_a_tablespace_whose_location_is_missing_is_noted: a
 * tablespace whose location is missing, its directory there without its
 * version directory, is a note, and nothing in it is checked.
 */
void
test_store_fsck_tests_a_tablespace_whose_location_is_missing_is_noted(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StdPathBuf	versioned = host_versioned_dir(HOST_PG_DEFAULT);
	StdStr		parent = host_directory_of(versioned.bytes.ptr, versioned.bytes.len);
	StdPathBuf	links;
	StdPathBuf	link;
	StdPathBuf	space = host_versioned_dir(77);
	StdStr		location = host_directory_of(space.bytes.ptr, space.bytes.len);
	StdIoError	io_error;
	StoreFsckFsck got;
	StoreFsckFsck want = store_fsck_tests_empty();
	StdFmtPiece pieces[] = {STD_FMT_LIT("tablespace 77's location \""), STD_FMT_ARG(0),
	STD_FMT_LIT("\" is missing, so the versioned files in it are not checked")};
	StdFmtArg	args[] = {{&location, std_path_Display_fmt}};
	StdFmtArguments arguments = {pieces, lengthof(pieces), args};

	std_path_join(parent.ptr, parent.len, "pg_tblspc", 9, &links);
	std_path_join(links.bytes.ptr, links.bytes.len, "77", 2, &link);
	store_fsck_tests_io_ok(std_fs_create_dir_all(link.bytes.ptr, link.bytes.len, &io_error), &io_error, 496, 51);
	store_fsck_push(&want.notes, &arguments);
	got = store_fsck_tests_checked(io);
	store_fsck_tests_fsck_eq(&got, &want, 502);
	std_vec_free_Checked(&space.bytes, 1, 1);
	std_vec_free_Checked(&link.bytes, 1, 1);
	std_vec_free_Checked(&links.bytes, 1, 1);
	std_vec_free_Checked(&versioned.bytes, 1, 1);
	simc_TestIo_drop(&test_io);
}

/*
 * test_store_fsck_tests_a_newest_identity_root_that_cant_be_read_is_found_with_its_range:
 * design item 10's identity tree: a newest identity tree whose root can't be
 * read is found as the base tree's is, and its fallback's range is one whose
 * gap is found.
 */
void
test_store_fsck_tests_a_newest_identity_root_that_cant_be_read_is_found_with_its_range(void)
{
	SimcTestIo	test_io = simc_io();
	HostIoIo   *io = &test_io.io;
	StoreFsckFsck fsck;

	store_fixtures_made(io, 935);
	store_fsck_tests_damage(935, store_fsck_tests_root(io, 935, true));
	fsck = store_fsck_tests_checked(io);
	store_fsck_tests_any_found(&fsck.found, "database 935: chunk ", strlen("database 935: chunk "), true, 514);
	store_fsck_tests_kept_assert(&fsck.kept, 935, 20, false, true, 515);
	store_fsck_Fsck_drop(&fsck);
	simc_TestIo_drop(&test_io);
}

#endif							/* DOLTGRES_TEST_STORE */
