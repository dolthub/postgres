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
 * read by, a decoded Tree or the TreeRef a writer just wrote, each its root
 * node's address and where the root was stored, the Fresh roots a store's
 * top-level writes return, the NodeStore and Branded tables stores read and
 * write through, and the MemoryStore that keeps nodes in memory.
 */
#ifndef DOLTGRES_TREES_STORE_H
#define DOLTGRES_TREES_STORE_H

#include "doltgres/chunks/address.h"
#include "doltgres/chunks/chunk.h"
#include "doltgres/chunks/error.h"
#include "doltgres/chunks/hash.h"
#include "doltgres/chunks/packs/backend.h"
#include "doltgres/chunks/packs/roots.h"
#include "doltgres/chunks/trailer.h"
#include "doltgres/host/cache.h"
#include "doltgres/host/io.h"
#include "doltgres/std/hashmap.h"
#include "doltgres/host/lock.h"
#include "doltgres/std/capability.h"
#include "doltgres/std/fmt.h"
#include "doltgres/std/sync.h"

/* TreesStoreTree is Tree: a tree's root node's address and where the root was stored, read from stored bytes. */
typedef struct TreesStoreTree
{
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
} TreesStoreTree;

/* TreesStoreTreeRef is TreeRef: the name of a chunk a writer just wrote, which never becomes a Tree. */
typedef struct TreesStoreTreeRef
{
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;
} TreesStoreTreeRef;

/* TreesStoreNamedOps is Named's table: parts, the root's address and where it was stored. */
typedef struct TreesStoreNamedOps
{
	void		(*parts) (void *self, ChunksAddressAddress *result_0, ChunksTrailerLocator *result_1);
} TreesStoreNamedOps;

/* TreesStoreNamed is a name a tree can be read by, a value that implements Named. */
typedef struct TreesStoreNamed
{
	const TreesStoreNamedOps *ops;
	void	   *self;
} TreesStoreNamed;

/* TreesStoreFresh is Fresh: a root a store's top-level write returned, which its brand alone may publish. */
typedef struct TreesStoreFresh
{
	TreesStoreTreeRef tree;
} TreesStoreFresh;

/* TreesNodeFlat is node.h's Flat, the flat bytes a store's read returns. */
struct TreesNodeFlat;

/* TreesShape is lib.h's Shape, the shape a branded writer's mutate takes. */
struct TreesShape;

/*
 * TreesStoreNodeStoreOps is NodeStore's table: read, read_record, write, as_dyn,
 * prefetch, and locate, with prefetch and locate defaulting to the trait's.
 */
typedef struct TreesStoreNodeStoreOps
{
	bool		(*read) (void *self, HostIoIo *io, const ChunksAddressAddress *address, ChunksTrailerLocator locator,
						 struct TreesNodeFlat *result, ChunksErrorError *error);
	bool		(*read_record) (void *self, HostIoIo *io, const ChunksAddressAddress *address,
								ChunksTrailerLocator locator, ChunksChunkChunk *result, ChunksErrorError *error);
	bool		(*write) (void *self, HostIoIo *io, StdVec *hashed, const ChunksTrailerLocator *locators,
						  size_t locators_len, const ChunksTrailerStamps *stamps, TreesStoreTreeRef *result,
						  ChunksErrorError *error);
	struct TreesStoreNodeStore (*as_dyn) (void *self);
	bool		(*prefetch) (void *self, HostIoIo *io, TreesStoreTree tree, ChunksErrorError *error);
	bool		(*locate) (void *self, HostIoIo *io, const ChunksAddressAddress *address, bool *some,
						   ChunksTrailerLocator *result, ChunksErrorError *error);
} TreesStoreNodeStoreOps;

/* TreesStoreNodeStore is a store of tree nodes, a value that implements NodeStore. */
typedef struct TreesStoreNodeStore
{
	const TreesStoreNodeStoreOps *ops;
	void	   *self;
} TreesStoreNodeStore;

/* TreesStoreBrandedOps is Branded's table: NodeStore's, then wrote. */
typedef struct TreesStoreBrandedOps
{
	TreesStoreNodeStoreOps node_store;
	bool		(*wrote) (void *self, ChunksTrailerLocator locator);
} TreesStoreBrandedOps;

/* TreesStoreBranded is a store whose top-level writes return roots it wrote itself, a value that implements Branded. */
typedef struct TreesStoreBranded
{
	const TreesStoreBrandedOps *ops;
	void	   *self;
} TreesStoreBranded;

/* TreesPublishedRepoId is RepoId: the repository whose packs a name's chunks are in, or a unique one of memory. */
typedef struct TreesPublishedRepoId
{
	bool		memory;
	uint64		id;
} TreesPublishedRepoId;

/*
 * TreesPublishedHeld is Held: a root a branded writer wrote, registered with
 * its packs as unpublished while the writer still writes it, which only that
 * writer's publish publishes.
 */
typedef struct TreesPublishedHeld
{
	TreesStoreFresh root;
	ChunksPacksRootsPin pin;
	TreesPublishedRepoId repo;
} TreesPublishedHeld;

/*
 * TreesPublishedPublished is Published: a tree name a writer published,
 * carrying the registration that keeps it a root until something durable
 * names it, or one read back from storage, which carries none.
 */
typedef struct TreesPublishedPublished
{
	ChunksAddressAddress address;
	ChunksTrailerLocator locator;

	/*
	 * pin is the Arc<Pin> of the registration the name carries, or NULL for a
	 * name that carries none.
	 */
	StdArcInner *pin;
	/* The repository the name entered from, whose doors alone take it. */
	TreesPublishedRepoId repo;
} TreesPublishedPublished;

/*
 * TreesStoreMemoryStore is MemoryStore: nodes kept in memory, each node's
 * flat bytes and each record by address, the writes counted, and its own
 * repository id.
 */
typedef struct TreesStoreMemoryStore
{
	/*
	 * nodes is each node's flat bytes, a HashMap<Address, Vec<u8>,
	 * AddressState>.
	 */
	ChunksHashHashMap nodes;
	/* records is each record chunk, a HashMap<Address, Chunk, AddressState>. */
	ChunksHashHashMap records;
	size_t		written;
	TreesPublishedRepoId repo;
} TreesStoreMemoryStore;

/*
 * TreesStoreReader is Reader: a reader of nodes from a repository's packs
 * through a node cache, the server's for a repository's trees or the
 * backend's own for its temporary trees.
 */
typedef struct TreesStoreReader
{
	const ChunksPacksBackendPacks *packs;
	HostCachePlacement placement;

	/*
	 * The published epoch of whoever reads, which keeps the packs' files it
	 * reads from being removed under it.
	 */
	const HostSharedReader *reading;
} TreesStoreReader;

/* TreesStoreLocated is Located: where the last copy of a chunk a writer wrote lies in its pack, with the xxh3 of its stamps, and whether a copy before it held other stamps. */
typedef struct TreesStoreLocated
{
	ChunksTrailerLocator last;
	uint64		sum;
	bool		several;
} TreesStoreLocated;

/* trees_TupleAddressVecU8VecU8_t is Logged, a node a logging writer wrote: its address, its hashed bytes, and its stamps' trailer. */
typedef struct trees_TupleAddressVecU8VecU8_t
{
	ChunksAddressAddress f0;
	StdVec		f1;
	StdVec		f2;
} trees_TupleAddressVecU8VecU8_t;

/* The IoMutex<HashMap<Locator, Vec<u8>, StdState>> of a temporary writer's decoded nodes. */
#define HOST_LOCK_SCOPE extern
#define HOST_LOCK_DECLARE
#define HOST_LOCK_TAKEN_TS Decoded
#define HOST_LOCK_TAKEN_T StdHashMap
#define HOST_LOCK_TAKEN_TYPE TREES_STORE_DECODED_LOCK_TYPE
#define HOST_LOCK_IO_MUTEX
#define HOST_LOCK_TS Decoded
#define HOST_LOCK_T StdHashMap
#define HOST_LOCK_DEFAULT(place) std_HashMap_Default_default((place), &TREES_STORE_HASH_MAP_LOCATOR_VEC_U8_TYPE)
#include "doltgres/host/lock_template.h"

/* TreesStorePackWriter is a writer's pack being written: in its repository's directory, or a temporary writer's scratch file. */
typedef union TreesStorePackWriter
{
	ChunksPackPackWriterInDir in_dir;
	ChunksPackPackWriterScratchFile scratch;
} TreesStorePackWriter;

/*
 * TreesStoreWriterCore is what every Writer<B> holds: a writer of nodes into
 * new packs, which reads existing nodes through a reader and nodes it wrote
 * back from the pack it writes until that pack is finished, closing a pack
 * that reaches the target size and starting another.
 */
typedef struct TreesStoreWriterCore
{
	TreesStoreReader reader;
	/* The tablespace whose directory the packs go in. */
	uint32		space;

	/*
	 * Whether the writer is temporary, its pack its own in a scratch file
	 * that no repository adds (B::File::SCRATCH).
	 */
	bool		scratch;

	/*
	 * Whether the writer has the pack being written, which finishing it
	 * takes.
	 */
	bool		has_pack;
	TreesStorePackWriter pack;

	/*
	 * located is a HashMap<Address, Located, AddressState>: where the last
	 * copy of each chunk written to the pack being written is in it.
	 */
	StdHashMap	located;

	/*
	 * For a temporary writer, the flat bytes of the nodes of its pack decoded
	 * so far, by where each copy is.
	 */
	HostLockIoMutexDecoded decoded;
	/* Whether every pack the writer writes trains a dictionary of its own. */
	bool		trains;

	/*
	 * Whether every pack the writer writes shares `shared`, a dictionary
	 * trained once for all that share it.
	 */
	bool		has_shared;
	ChunksPackSharedDictionary shared;

	/*
	 * closed is a Vec<Writing>: the packs it closed at the target size, which
	 * stay among the packs being written until it is done.
	 */
	StdVec		closed;
	/* finished is a Vec<PackId>: the ids of the packs it finished, in order. */
	StdVec		finished;

	/*
	 * For a logging writer, `logged`, a Vec<Logged> of each node it wrote,
	 * children before parents.
	 */
	bool		has_logged;
	StdVec		logged;
} TreesStoreWriterCore;

/* TreesStoreWriter is Writer<Loose>: a writer whose roots others' roots name, which its brand turns into a branded one. */
typedef struct TreesStoreWriter
{
	TreesStoreWriterCore core;
} TreesStoreWriter;

/* TreesStoreTemporaryWriter is Writer<Scratch>: a temporary writer, its pack its own in a scratch file, whose roots never publish. */
typedef struct TreesStoreTemporaryWriter
{
	TreesStoreWriterCore core;
} TreesStoreTemporaryWriter;

/* TreesStoreOptionFresh is an Option<Fresh>: a branded writer's root at its place, until a hold moves it out. */
typedef struct TreesStoreOptionFresh
{
	bool		has;
	TreesStoreFresh value;
} TreesStoreOptionFresh;

/*
 * TreesStoreBrandedWriter is Writer<Id>: a writer whose roots from its brand
 * on it holds and publishes, which alone has hold and publish, holding each
 * root it holds itself, so that its publish publishes its own roots and no
 * other writer's.
 */
typedef struct TreesStoreBrandedWriter
{
	TreesStoreWriterCore core;

	/*
	 * fresh is a Vec<Option<Fresh>>: the roots the writer's own top-level
	 * writes returned, in order, which hold names by their place and moves
	 * out.
	 */
	StdVec		fresh;

	/*
	 * held is a Vec<Held>: the roots the writer holds, in the order it held
	 * them.
	 */
	StdVec		held;
} TreesStoreBrandedWriter;

/* TREES_STORE_WRITER_NODE_STORE coerces a Writer of any brand, by its core, to the NodeStore it implements. */
#define TREES_STORE_WRITER_NODE_STORE(core) ((TreesStoreNodeStore) {&TREES_STORE_WRITER_BRANDED_OPS.node_store, (core)})

/* TREES_STORE_WRITER_BRANDED coerces a plain Writer to the Branded it implements, its core's. */
#define TREES_STORE_WRITER_BRANDED(x) ((TreesStoreBranded) {&TREES_STORE_WRITER_BRANDED_OPS, &(x)->core})

/* TREES_STORE_TEMPORARY_WRITER_BRANDED coerces a temporary Writer to the Branded it implements, its core's. */
#define TREES_STORE_TEMPORARY_WRITER_BRANDED(x) ((TreesStoreBranded) {&TREES_STORE_WRITER_BRANDED_OPS, &(x)->core})

/* TREES_STORE_TREE_NAMED coerces a Tree to the Named it implements. */
#define TREES_STORE_TREE_NAMED(x) ((TreesStoreNamed) {&TREES_STORE_TREE_NAMED_OPS, (x)})

/* TREES_STORE_TREE_REF_NAMED coerces a TreeRef to the Named it implements. */
#define TREES_STORE_TREE_REF_NAMED(x) ((TreesStoreNamed) {&TREES_STORE_TREE_REF_NAMED_OPS, (x)})

/* TREES_STORE_FRESH_NAMED coerces a Fresh to the Named it implements. */
#define TREES_STORE_FRESH_NAMED(x) ((TreesStoreNamed) {&TREES_STORE_FRESH_NAMED_OPS, (x)})

/* TREES_STORE_MEMORY_STORE_NODE_STORE coerces a MemoryStore to the NodeStore it implements. */
#define TREES_STORE_MEMORY_STORE_NODE_STORE(x) \
	((TreesStoreNodeStore) {&TREES_STORE_MEMORY_STORE_BRANDED_OPS.node_store, (x)})

/* TREES_STORE_MEMORY_STORE_BRANDED coerces a MemoryStore to the Branded it implements. */
#define TREES_STORE_MEMORY_STORE_BRANDED(x) ((TreesStoreBranded) {&TREES_STORE_MEMORY_STORE_BRANDED_OPS, (x)})

/* TREES_STORE_READER_NODE_STORE coerces a Reader to the NodeStore it implements. */
#define TREES_STORE_READER_NODE_STORE(x) ((TreesStoreNodeStore) {&TREES_STORE_READER_NODE_STORE_OPS, (x)})



extern const TreesStoreNamedOps TREES_STORE_TREE_NAMED_OPS;
extern const TreesStoreNamedOps TREES_STORE_TREE_REF_NAMED_OPS;

extern TreesStoreTree trees_store_Tree_decoded(ChunksAddressAddress address, ChunksTrailerLocator locator);
extern TreesStoreTreeRef trees_store_Tree_as_ref(const TreesStoreTree *self);
extern TreesStoreTree trees_store_Tree_at(ChunksAddressAddress address, ChunksTrailerLocator locator);
extern bool trees_store_Sealed_Debug_fmt(const void *value, StdFormatter *f);
extern bool trees_store_Tree_Debug_fmt(const void *value, StdFormatter *f);
extern bool trees_store_Tree_PartialEq_eq(const void *self_, const void *other_);
extern void trees_store_Tree_Hash_hash(const void *self_, StdHasher state);
extern bool trees_store_TreeRef_PartialEq_eq(const void *self_, const void *other_);
extern bool trees_store_Tree_Display_fmt(const void *value, StdFormatter *f);
extern TreesStoreTree trees_store_name(TreesStoreNamed named);
extern TreesStoreTreeRef trees_store_TreeRef_written(ChunksAddressAddress address, ChunksTrailerLocator locator);
extern TreesStoreTreeRef trees_store_TreeRef_of(TreesStoreTree tree);
extern TreesStoreTree trees_store_TreeRef_tree(const TreesStoreTreeRef *self);
extern bool trees_store_TreeRef_Debug_fmt(const void *value, StdFormatter *f);
extern bool trees_store_TreeRef_Display_fmt(const void *value, StdFormatter *f);

extern const TreesStoreNamedOps TREES_STORE_FRESH_NAMED_OPS;
extern const TreesStoreBrandedOps TREES_STORE_MEMORY_STORE_BRANDED_OPS;

extern TreesStoreFresh trees_store_Fresh_of(TreesStoreTree tree);
extern TreesStoreTreeRef trees_store_Fresh_as_ref(const TreesStoreFresh *self);
extern bool trees_store_rooted(HostIoIo *io, TreesStoreBranded store, TreesStoreTree tree, TreesStoreFresh *result,
							   ChunksErrorError *error);
extern bool trees_store_write_root(HostIoIo *io, TreesStoreBranded store, StdVec *hashed,
								   const ChunksTrailerLocator *locators, size_t locators_len,
								   const ChunksTrailerStamps *stamps, TreesStoreFresh *result, ChunksErrorError *error);
extern bool trees_store_NodeStore_prefetch(void *self, HostIoIo *io, TreesStoreTree tree, ChunksErrorError *error);
extern bool trees_store_NodeStore_locate(void *self, HostIoIo *io, const ChunksAddressAddress *address, bool *some,
										 ChunksTrailerLocator *result, ChunksErrorError *error);
extern bool trees_store_stamps_of(const ChunksChunkChunk *chunk, ChunksTrailerStamps *result, ChunksErrorError *error);
extern bool trees_store_cached_stamps(HostIoIo *io, TreesStoreNodeStore store, TreesStoreNamed tree,
									  StdArcInner **result, ChunksErrorError *error);
extern TreesStoreMemoryStore trees_store_MemoryStore_Default_default(void);
extern TreesStoreMemoryStore trees_store_MemoryStore_new(void);
extern TreesPublishedRepoId trees_store_MemoryStore_repo(const TreesStoreMemoryStore *self);
extern size_t trees_store_MemoryStore_written(const TreesStoreMemoryStore *self);
extern size_t trees_store_MemoryStore_len(const TreesStoreMemoryStore *self);
extern bool trees_store_MemoryStore_is_empty(const TreesStoreMemoryStore *self);
extern bool trees_store_VecU8_PartialEq_eq(const void *a, const void *b);
extern void trees_store_MemoryStore_drop(TreesStoreMemoryStore *self);

extern const TreesStoreNodeStoreOps TREES_STORE_READER_NODE_STORE_OPS;
extern bool trees_store_Reader_read(const TreesStoreReader *self, HostIoIo *io, const ChunksAddressAddress *address,
									ChunksTrailerLocator locator, struct TreesNodeFlat *result,
									ChunksErrorError *error);
extern bool trees_store_Reader_read_record(const TreesStoreReader *self, HostIoIo *io,
										   const ChunksAddressAddress *address, ChunksTrailerLocator locator,
										   ChunksChunkChunk *result, ChunksErrorError *error);
extern const TreesStoreBrandedOps TREES_STORE_WRITER_BRANDED_OPS;
extern bool trees_store_Writer_new(HostIoIo *io, TreesStoreReader reader, uint32 space, TreesStoreWriter *result,
								   ChunksErrorError *error);
extern bool trees_store_Writer_temporary(HostIoIo *io, TreesStoreReader reader, TreesStoreTemporaryWriter *result,
										 ChunksErrorError *error);
extern void trees_store_Writer_logging(TreesStoreWriter *self);
extern bool trees_store_Writer_finish(TreesStoreWriter *self, HostIoTakesShort io, StdVec *result,
									  ChunksErrorError *error);
extern void trees_store_Writer_brand(TreesStoreWriter *self, TreesStoreBrandedWriter *result);
extern bool trees_store_BrandedWriter_empty(TreesStoreBrandedWriter *self, HostIoIo *io, size_t *result,
											ChunksErrorError *error);
extern bool trees_store_BrandedWriter_mutate(TreesStoreBrandedWriter *self, HostIoIo *io, TreesStoreNamed tree,
											 uint32 target, StdVec *edits, const struct TreesShape *shape,
											 size_t *result, ChunksErrorError *error);
extern bool trees_store_BrandedWriter_write_root(TreesStoreBrandedWriter *self, HostIoIo *io, StdVec *hashed,
												 const ChunksTrailerLocator *locators, size_t locators_len,
												 const ChunksTrailerStamps *stamps, size_t *result,
												 ChunksErrorError *error);
extern TreesStoreTreeRef trees_store_BrandedWriter_root(const TreesStoreBrandedWriter *self, size_t root);
extern bool trees_store_BrandedWriter_hold(TreesStoreBrandedWriter *self, HostIoTakesShort io,
										   ChunksPacksRootsRootSlot *slot, size_t root, ChunksErrorError *error)
			STD_RELEASE(*slot);
extern bool trees_store_BrandedWriter_publish(TreesStoreBrandedWriter *self, HostIoTakesShort io, StdVec *published,
											  StdVec *finished, ChunksErrorError *error);
extern void trees_store_BrandedWriter_drop(TreesStoreBrandedWriter *self);
extern void trees_store_Writer_logged(TreesStoreWriterCore *self, StdVec *result);
extern void trees_store_Writer_train_dictionaries(TreesStoreWriterCore *self);
extern void trees_store_Writer_share_dictionary(TreesStoreWriterCore *self, ChunksPackSharedDictionary shared);
extern TreesStoreReader trees_store_Writer_reader(const TreesStoreWriterCore *self);
extern uint32 trees_store_Writer_space(const TreesStoreWriterCore *self);
extern bool trees_store_Writer_holds(const TreesStoreWriterCore *self, const ChunksAddressAddress *address);
extern void trees_store_Writer_drop(TreesStoreWriter *self);
extern void trees_store_TemporaryWriter_drop(TreesStoreTemporaryWriter *self);
extern void trees_store_Logged_drop(trees_TupleAddressVecU8VecU8_t *self);

#ifdef DOLTGRES_TEST_TREES
extern void test_trees_store_tests_a_leaf_of_long_keys_sharing_a_prefix_stays_bounded_once_decoded(void);
extern void test_trees_store_tests_a_writers_read_by_address_alone_of_a_chunk_it_wrote_with_other_stamps_is_refused_and_of_equal_copies_answers(void);
extern void test_trees_store_tests_a_writers_failed_read_of_its_own_copy_returns_the_hosts_error_and_not_the_last_copy(void);
extern void test_trees_store_tests_a_writers_next_pack_trains_or_shares_a_dictionary_as_its_packs_do(void);
extern void test_trees_store_tests_copies_in_packs_with_wide_numbers_have_their_own_keys(void);
#endif

#endif							/* DOLTGRES_TREES_STORE_H */
