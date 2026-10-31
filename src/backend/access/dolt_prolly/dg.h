#ifndef DOLT_PROLLY_PG_H
#define DOLT_PROLLY_PG_H
#include "postgres.h"
#include "access/tableam.h"
#include "access/table.h"
#include "nodes/execnodes.h"
#include "commands/defrem.h"
#include "access/genam.h"
#include "executor/tuptable.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/jsonb.h"
#include "../../../doltgres/prolly/internal.h"

struct HandleDelta;
typedef struct DGBatch DGBatch;

/*
 * A relation's tree as one caller sees it. States from dg_load share the
 * transaction's working store, which loads missing nodes from the chunk store;
 * other states own a private store. The content tree maps row digest ->
 * count + row for a table, index key -> count + row for an index. A table's
 * row handles are derived from its content, with a delta of allocated ones
 * read from the catalog, or private to the state: see dg_handle.inc.
 */
typedef struct DGState
{
	PLStore    *store;
	bool		has_root;
	PLTree		content;
	uint64		nextid;			/* next handle, for private states only */
	bool		shared;			/* store is the transaction's working store */
	bool		based;			/* derived from committed roots */
	PLHash		base;			/* that committed content root */
	MemoryContextCallback cleanup;
	int64	   *journal_seqs;	/* journal entries folded into the tree, ascending */
	int			journal_count;
	uint64		roots_generation;	/* of its roots' storage, just before they were read */
	bool		roots_generation_known;
	struct HandleDelta *delta;	/* allocated handles, by handle and by digest */
	bool		delta_private;	/* the delta belongs to this state, not the catalog */
	struct HandleDelta *extra;	/* occurrences the delta does not name, numbered for this state */
} DGState;

/* Handles below this are derived from content; the counter allocates above it. */
#define DG_DERIVED_HANDLES (UINT64CONST(1) << 40)

/* Handles from this up name occurrences a state numbered for itself, near the top of the TID space. */
#define DG_SYNTHETIC_HANDLES UINT64CONST(1240000000000)
typedef struct DGRows
{
	PLEntry    *items;
	size_t		count;
	size_t		capacity;
}			DGRows;
typedef struct DGStorage DGStorage;
extern struct _SPI_plan *dg_manifest_plan(const char *sql, int nargs, Oid *types);
extern bool dg_object_read(bytea *hash, const char *kind, char **actual, bytea **payload, struct ArrayType **children);
extern Oid	dg_storage_allocate(Relation definition, const char *branch);
extern Oid	dg_storage_allocate_kind(char relkind, const char *branch);
extern bool dg_storage_content_root(RelFileLocator locator, const char *branch, Snapshot snapshot, PLHash *content);
extern bool dg_storage_resource_root(Oid resource, const char *branch, Snapshot snapshot, PLHash *content, bool *load);
extern bool dg_journal_pending(const char *branch, Snapshot snapshot);
extern bytea *dg_manifest_put(const char *kind, bytea *payload, struct ArrayType *children);

/* A member of a database object: its hash, kind and, unless a namespace, name. */
typedef struct DGMember
{
	bytea	   *hash;
	const char *kind;
	const char *name;
} DGMember;
extern DGMember *dg_database_members(bytea *database, int *count);
extern bytea *dg_database_member(bytea *database, const char *kind, const char *name);
extern bytea *dg_member_child(bytea *hash, int index, bool *known);
extern void dg_initialize_resource(Relation rel, const char *branch);
extern void dg_storage_record(Relation source);
extern DGStorage *dg_storage_open(Oid definition, const char *branch, const char *name, bytea *schema, Oid resource, Snapshot snapshot);
extern DGState *dg_storage_load(DGStorage *storage);
extern void dg_storage_close(DGStorage *storage);
extern void dg_storage_lock_branch(Oid resource, const char *branch, LOCKMODE mode);
extern void dg_storage_lock_working(const char *branch, Snapshot snapshot);
extern Relation dg_catalog_write_table(const char *branch, const char *name, AclMode permission);
extern Relation dg_storage_enable_read(DGStorage *storage, bytea *base);
extern Relation dg_storage_enable_write(DGStorage *storage, bytea *base);
extern void dg_storage_close_relation(Relation rel);
extern void dg_storage_set_indexes(Relation rel, List *indexes);
extern List *dg_storage_indexes(Relation rel);
extern Relation dg_storage_index(Relation rel, Oid id);
extern const char *dg_storage_branch(Relation rel);
extern void dg_storage_require_write(Relation rel);
extern bool dg_storage_bound(Relation rel);
extern void dg_storage_table_locator(Relation index, RelFileLocator *locator);
extern DGState *dg_storage_am_load(Relation rel, Snapshot snapshot);
extern DGState *dg_storage_am_load_tid(Relation rel, uint64 id);
extern DGState *dg_load_bound_tid(Relation rel, uint64 id, const char *branch);
extern DGState *dg_load_bound(Relation rel, Snapshot snapshot, const char *branch);
extern void dg_check(PLStatus status);
extern PLHash dg_rowhash(PLSlice row);
extern DGState *dg_load(Relation rel, Snapshot snapshot);
extern DGState *dg_catalog_mutation_load(Relation rel, Snapshot snapshot);
extern void dg_catalog_mutation_release(DGState *state);
extern bool dg_catalog_begin_rewrite(Relation definition);
extern void dg_catalog_finish_rewrite(Relation definition);
extern DGState *dg_catalog_rewrite_state(Relation definition);
extern void dg_decode_mutation(Relation rel, PLSlice bytes, ItemPointer tid, TupleTableSlot *slot);
extern DGState *dg_load_branch(Relation rel, Snapshot snapshot, const char *branch);
extern DGState *dg_load_tid(Relation rel, uint64 id);
extern void dg_save(Relation rel, DGState *state, CommandId cid);
extern uint64 dg_allocate_handle(Relation rel);
extern Oid	dg_identity_lookup(const char *branch, char kind, const char *name);
extern bool dg_identity_describe(Oid oid, char **branch, char *kind, char **name);
extern Oid	dg_identity_ensure(const char *branch, char kind, const char *name);
extern void dg_identity_invalidate_branch(const char *branch);
extern void dg_identity_delete_branch(const char *branch);
extern void dg_identity_sync(const char *branch, List *members, List *tables);
extern void dg_note_modified(Relation rel, const char *branch, uint64 handle, CommandId cid);
extern bool dg_row_modified(Relation rel, const char *branch, uint64 handle, CommandId *cid);
extern bool dg_row_committed(Relation rel, uint64 handle);
extern bool dg_handle_digest(DGState *s, uint64 handle, PLHash *digest);
extern int	dg_handles_of(DGState *s, PLHash digest, int64 count, uint64 **handles, int *capacity);
extern double dg_handle_count(DGState *s);
extern uint64 dg_handle_choose(Relation rel, DGState *s, DGBatch *b, PLHash digest);
extern void dg_handle_note(DGState *s, uint64 handle, PLHash digest, bool present);
extern void dg_handle_install(Relation rel, DGState *s, PLTree contents);
extern bool dg_handle_version(Relation rel, uint64 handle, PLHash *digest, PLSlice *row);
extern bool dg_handle_range(uint64 first, uint64 count, PLHash *lower, PLHash *upper);
/* What lock records say of a handle: none, a live one, or only ones deleted for good. */
#define DG_RECORD_NONE 0
#define DG_RECORD_LIVE 1
#define DG_RECORD_DEAD 2
extern int	dg_record_status(RelFileLocator *locator, const char *branch, int64 id);
extern void dg_record_renew(RelFileLocator *locator, const char *branch, int64 id);
extern bool dg_records_exist(Oid space, RelFileNumber file);
extern void dg_handle_records_note(RelFileLocator *locator);
extern void dg_serializable_write(Relation rel);
extern TM_Result dg_record_lock(Relation rel, const char *branch, int64 id, CommandId cid, LockTupleMode mode,
								LockWaitPolicy wait, bool follow, TM_FailureData *tmfd, int64 *next);
extern TM_Result dg_record_update(Relation rel, const char *branch, int64 id, int64 next_id, CommandId cid,
								  bool key_change, bool wait, TM_FailureData *tmfd, int64 *next);
extern int64 dg_record_successor(Relation rel, const char *branch, int64 id);
extern TransactionId dg_record_xwait(Relation rel, const char *branch, int64 id, bool lockers);
extern TM_Result dg_record_delete(Relation rel, const char *branch, int64 id, CommandId cid, bool wait,
								  bool changing_part, TM_FailureData *tmfd, int64 *next);

/* An index entry's key for |row|, as the index AM encodes it. */
extern bool dg_index_row_matches(Relation table, Relation index, PLSlice row);
extern bool dg_index_row_key(Relation table, Relation index, PLSlice row, StringInfo key, bool *nullkey);
extern bool dg_index_entry_key(Relation table, Relation index, PLSlice row, PLHash digest, StringInfo key);

/* Unique-key reservations; see dg_lock.c. */
extern bool dg_key_reserve(Relation index, const char *branch, uint64 hash, bool wait);
extern TransactionId dg_key_reserver(Relation index, const char *branch, uint64 hash);

/* The table slot: rows decoded a column at a time as asked for; see dg_codec.c. */
extern const TupleTableSlotOps TTSOpsProlly;

/* Rows an index scan found, for the table fetches that follow; see dg_table.c. */
extern void dg_stash_put(Oid relid, const DGState *state, uint64 handle, PLSlice row);
extern bool dg_stash_get(Oid relid, const DGState *state, uint64 handle, PLSlice *row);

/* A command's pending edits to one relation's trees; see dg_batch.inc. */
extern DGBatch *dg_batch(Relation rel, CommandId cid);
extern DGBatch *dg_batch_current(Relation rel);
extern DGBatch *dg_batch_fresh(Relation rel);
extern DGBatch *dg_batch_latest(Relation rel, CommandId cid);
extern DGState *dg_batch_state(DGBatch *batch);
extern void dg_batch_insert(DGBatch *batch, PLSlice row, uint64 handle);
extern void dg_batch_remove(DGBatch *batch, PLSlice row, uint64 handle);
extern void dg_batch_index(DGBatch *batch, PLSlice key, size_t prefix, PLHash digest, PLSlice row, int64 delta);
extern void dg_batch_index_prefix(DGBatch *batch, PLSlice prefix, List **added, List **removed);
extern bool dg_batch_handle_digest(DGBatch *batch, uint64 handle, PLHash *digest);
extern bool dg_batch_row(DGBatch *batch, PLHash digest, PLSlice *row);
extern int	dg_batch_handles(DGBatch *batch, PLHash digest, uint64 **handles);
extern bool dg_batch_blocks_handle(DGBatch *batch, PLHash digest, uint64 handle);
extern void dg_batch_finish(Relation rel);
extern DGState *dg_batch_load(Relation rel);
extern PLStore *dg_working_store(void);
extern PLStore *dg_private_store(void);
extern void dg_collect(void);
extern bool dg_version_call_active(void);
extern bool dg_native_rewrite_active(void);
extern void dg_release(DGState *state);
extern void dg_branch_base(Relation rel, DGState *state);
extern bytea *dg_schema_store(const char *kind, const char *json);
extern char *dg_schema_read(bytea *address, const char *kind);
extern char *dg_schema_decode(bytea *address, const char *kind);

/*
 * A schema object decoded once, by address; see dg_schema_cache.c. An address
 * names immutable content, so an entry is never invalidated, only evicted, and
 * what is derived from its definition alone is kept with it. Callers must not
 * change an entry, which stays valid until the transaction ends.
 */
typedef struct DGSchema
{
	char	   *json;			/* the definition */
	Jsonb	   *document;		/* json, parsed */
	uint32		parts;			/* DG_SCHEMA_* parts derived so far */
	TupleDesc	columns;		/* a table's or index's columns, with built-in types */
	int			ndefaults;
	Node	  **defaults;		/* each column's raw default, or NULL */
	int			nconstraints;
	Node	  **constraints;	/* each table constraint's raw Constraint */
	Node	   *index;			/* an index's raw IndexStmt */
	int			nopclasses;
	Oid		   *opclass_types;	/* each index key's built-in type */
	Oid		   *opclasses;		/* and the operator class resolved for it */
} DGSchema;

#define DG_SCHEMA_COLUMNS		0x01
#define DG_SCHEMA_DEFAULTS		0x02
#define DG_SCHEMA_CONSTRAINTS	0x04
#define DG_SCHEMA_INDEX			0x08
#define DG_SCHEMA_OPCLASSES		0x10

extern DGSchema *dg_schema_get(bytea *address, const char *kind);
extern DGSchema *dg_schema_find(const char *json);
extern void dg_schema_set_columns(DGSchema *schema, TupleDesc columns);
extern void dg_schema_set_nodes(DGSchema *schema, uint32 part, int count, Node **nodes);
extern void dg_schema_set_opclasses(DGSchema *schema, int count, Oid *types, Oid *opclasses);
extern bytea *dg_history_export_tree(const PLTree * tree);
extern PLTree dg_history_import(PLStore *store, bytea *address);
extern void dg_history_install(Relation rel, DGState *state, PLTree contents);
extern DGRows dg_rows(const PLTree * tree);
extern void dg_tid(uint64 id, ItemPointer tid);
extern uint64 dg_id(ItemPointer tid);
extern PLSlice dg_encode(Relation rel, TupleTableSlot *slot);
/* A prolly relation holding rows: a table, or a materialized view's table. */
#define DG_ROWS_KIND(relkind) ((relkind) == RELKIND_RELATION || (relkind) == RELKIND_MATVIEW)

extern PLSlice dg_encode_value(Oid type, Datum value);
extern Datum dg_decode_value(Oid type, PLSlice bytes);

typedef struct DGConversion
{
	Oid			from;
	Oid			to;
	struct ParamListInfoData *params;
	struct ExprState *state;
	struct ExprContext *context;
} DGConversion;

extern DGConversion *dg_conversion_begin(Oid from, Oid to, int32 typmod);
extern PLSlice dg_conversion_apply(DGConversion *conversion, PLSlice bytes);
extern PLSlice dg_recode_label(Oid type, PLSlice bytes, Oid target, const char *old, const char *renamed);
extern PLSlice dg_recode_composite(Oid type, PLSlice bytes, Oid target, int count, const int *mapping);
extern bool dg_key_ordered(Oid type);
extern PLSlice dg_key_value(Oid type, Datum value);
extern PLSlice dg_key_field(Oid type, PLSlice bytes);
extern void dg_index_apply(Relation table, PLSlice row, int64 delta, uint64 handle, CommandId cid);
extern bool dg_index_check_unique(Relation table, Relation index, ItemPointer tid, Datum *values, bool *isnull,
								  IndexUniqueCheck check);
extern void dg_index_validate(Relation table, Relation index);
extern HeapTuple dg_decode_descriptor(TupleDesc desc, PLSlice bytes);
extern void dg_decode(Relation rel, PLSlice tuple, ItemPointer tid, TupleTableSlot *slot);
extern void dg_validate_descriptor(TupleDesc desc);
extern void dg_rebuild_indexes(Relation rel);
extern void dg_index_rebuild(Relation table, Relation index);
extern const TableAmRoutine *dg_table_routine(void);
extern pg_noreturn void dg_unsupported(const char *operation);

/*
 * Identifier quoting for canonical names and SQL. Unlike quote_identifier,
 * these ignore quote_all_identifiers: canonical bytes never depend on a GUC.
 */
extern const char *dg_quote_identifier(const char *ident);
extern char *dg_qualified_name(const char *qualifier, const char *ident);
extern void dg_flush_version_batches(void);
#endif
