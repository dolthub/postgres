/* ---------------------------------------------------------------------------
 * Pending edits: a command's writes to one relation, applied in one pass
 * ---------------------------------------------------------------------------
 *
 * The write path records a command's edits here, against the state it
 * loaded, and appends them to the delta store, one version per key and tree,
 * when the trees must be read by anything else: a load of the same roots,
 * the end of a bulk insert, a subtransaction boundary, or commit. The
 * command's own writer reads through the pending edits, so uniqueness checks
 * see the rows the same statement inserted.
 */

typedef struct PendingRow
{
	PLHash		digest;			/* hash key */
	int64		delta;			/* occurrences added, less removed */
	PLSlice		row;			/* the row bytes, in the batch's memory */
	uint64	   *handles;		/* handles added for it by this batch */
	int			handle_count;
} PendingRow;

typedef struct PendingHandle
{
	uint64		handle;			/* hash key */
	PLHash		digest;
	bool		present;
} PendingHandle;

/* An index entry: its key, as index_entry encodes it, ending in its row's digest. */
typedef struct PendingEntry
{
	PLSlice		key;
	PLHash		digest;			/* of the row, as the key's escaped tail encodes */
	int64		delta;
} PendingEntry;

/* Entries of a unique index sharing a key prefix, found by the prefix's hash. */
typedef struct PendingPrefix
{
	uint64		hash;			/* hash key */
	List	   *entries;		/* PendingEntry pointers */
} PendingPrefix;

struct DGBatch
{
	RootKey		key;			/* hash key */
	Oid			relid;			/* reopened to apply the edits ... */
	Relation	bound;			/* ... unless a private storage descriptor */
	CommandId	cid;
	SubTransactionId subxid;
	uint64		generation;		/* roots generation the state was loaded under */
	uint64		words[2];		/* the storage's words then: see storage_word */
	Oid			table_db,		/* the table's storage */
				table_space;
	RelFileNumber table_file;
	bool		table;
	bool		on_snapshot;	/* its state is the transaction snapshot's (on_snapshot) */
	MemoryContext memory;
	DGState    *state;			/* the trees the edits apply to */
	HTAB	   *rows;			/* PendingRow by digest */
	HTAB	   *handles;		/* PendingHandle by handle */
	PendingEntry **entries;		/* an index's edits, in arrival order */
	int			entry_count,
				entry_capacity;
	HTAB	   *prefixes;		/* PendingPrefix by hash of the key prefix */
	int			count;			/* edits recorded */
};

static void batch_stamp(DGBatch *b);
static bool batch_stale(DGBatch *b);

static PLSlice
slice_copy(PLSlice bytes)
{
	uint8	   *copy = palloc(Max(bytes.len, 1));

	memcpy(copy, bytes.data, bytes.len);
	return (PLSlice) {copy, bytes.len};
}

static DGBatch *
batch_find(const RootKey *key)
{
	if (!batches)
		return NULL;
	return hash_search(batches, key, HASH_FIND, NULL);
}

static HTAB *
batch_table(MemoryContext memory, const char *name, Size keysize, Size entrysize)
{
	HASHCTL		ctl = {0};

	ctl.keysize = keysize;
	ctl.entrysize = entrysize;
	ctl.hcxt = memory;
	return hash_create(name, 256, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static int
compare_entries_by_key(const void *a, const void *b)
{
	return pl_compare((*(PendingEntry *const *) a)->key, (*(PendingEntry *const *) b)->key);
}

/* The count stored for |key| in |s|, and whether an entry exists. */
static int64
tree_count(DGState *s, PLSlice key, PLValue *value)
{
	dg_get(s, key, value);
	return value->present ? (int64) pl_read64(value->bytes.data) : 0;
}

/* Append a table batch's edits to its tree's delta. */
static void
batch_flush_table(DGBatch *b)
{
	DGState    *s = b->state;
	HASH_SEQ_STATUS status;
	PendingRow *row;
	DoltDeltaEdit *edits;
	long		n;
	int			i;
	bool		check = !batch_stale(b);

	n = hash_get_num_entries(b->rows);
	edits = palloc(Max(n, 1) * sizeof(DoltDeltaEdit));
	i = 0;
	hash_seq_init(&status, b->rows);
	while ((row = hash_seq_search(&status)) != NULL)
	{
		PLValue		old;
		int64		count;

		if (row->delta == 0)
			continue;
		count = tree_count(s, (PLSlice) {row->digest.bytes, PL_HASH_SIZE}, &old) + row->delta;
		if (old.present && (old.bytes.len != row->row.len + 8 || memcmp(old.bytes.data + 8, row->row.data, row->row.len)))
			elog(ERROR, "canonical row hash collision");
		/* A batch older than later commits may remove rows they added. */
		if (count < 0 && check)
			elog(ERROR, "missing canonical row");
		edits[i].key = row->digest.bytes;
		edits[i].keylen = PL_HASH_SIZE;
		edits[i].row = row->row.data;
		edits[i].rowlen = row->row.len;
		edits[i].delta = row->delta;
		i++;
	}
	delta_append(&b->key, s->tree.root, edits, i, b->cid);
	pfree(edits);
	handle_delta_write(&b->key, b->handles, b->cid);
}

/* Append an index batch's edits to its tree's delta: entries with equal keys combine. */
static void
batch_flush_index(DGBatch *b)
{
	DGState    *s = b->state;
	PendingEntry **sorted = palloc(Max(b->entry_count, 1) * sizeof(PendingEntry *));
	DoltDeltaEdit *edits = palloc(Max(b->entry_count, 1) * sizeof(DoltDeltaEdit));
	int			n = 0;
	bool		check = !batch_stale(b);

	memcpy(sorted, b->entries, b->entry_count * sizeof(PendingEntry *));
	qsort(sorted, b->entry_count, sizeof(PendingEntry *), compare_entries_by_key);
	for (int i = 0; i < b->entry_count;)
	{
		PendingEntry *entry = sorted[i];
		int64		delta = 0;
		PLValue		old;

		for (; i < b->entry_count && pl_compare(sorted[i]->key, entry->key) == 0; i++)
			delta += sorted[i]->delta;
		if (delta == 0)
			continue;
		if (check && tree_count(s, entry->key, &old) + delta < 0)
			elog(ERROR, "prolly index is missing an entry");
		edits[n].key = entry->key.data;
		edits[n].keylen = entry->key.len;
		edits[n].row = NULL;
		edits[n].rowlen = 0;
		edits[n].delta = delta;
		n++;
	}
	delta_append(&b->key, s->tree.root, edits, n, b->cid);
	pfree(edits);
	pfree(sorted);
}

/*
 * The latest trees of |rel|: committed roots merged with this transaction's
 * overlay, without applying the current batch. Pending edits are deltas, so
 * they apply to whatever the trees hold when they are finally applied.
 */
static DGState *
batch_load(Relation rel)
{
	DGState    *state;

	loading_for_batch = true;
	PG_TRY();
	{
		state = dg_load(rel, SnapshotSelf);
	}
	PG_FINALLY();
	{
		loading_for_batch = false;
	}
	PG_END_TRY();
	return state;
}

/* Reload |b|'s state, opening |rel| for it. */
static void
batch_reload(DGBatch *b, Relation rel)
{
	MemoryContext old = MemoryContextSwitchTo(b->memory);

	/* The writer's lock on the relation outlives its statement. */
	batch_stamp(b);
	b->state = batch_load(rel);
	MemoryContextSwitchTo(old);
}

/* Append |b|'s edits to the delta store. */
static void
batch_apply(DGBatch *b)
{
	if (b->table)
		batch_flush_table(b);
	else
		batch_flush_index(b);
}

static void
batch_forget(DGBatch *b)
{
	RootKey		key = b->key;
	MemoryContext memory = b->memory;

	hash_search(batches, &key, HASH_REMOVE, NULL);

	/* The state's cleanup callback runs with the memory, releasing it. */
	MemoryContextDelete(memory);
}

/* The batches of the indexes of table batch |tb| from its command. */
static List *
batch_index_group(DGBatch *tb)
{
	HASH_SEQ_STATUS status;
	DGBatch    *b;
	List	   *group = NIL;

	hash_seq_init(&status, batches);
	while ((b = hash_seq_search(&status)) != NULL)
		if (!b->table && b->count && b->cid == tb->cid && b->subxid == tb->subxid &&
			b->table_space == tb->key.space && b->table_file == tb->key.file &&
			strcmp(b->key.branch, tb->key.branch) == 0)
			group = lappend(group, b);
	return group;
}

/*
 * Apply table batch |b| with its indexes' batches from the same command: the
 * indexes apply at once, before anything else can load them, and if any
 * state is stale they all reload together under one snapshot.
 */
static void
batch_apply_group(DGBatch *b, Relation rel)
{
	List	   *group = batch_index_group(b);
	List	   *rels = NIL;
	bool		stale = batch_stale(b);
	ListCell   *lc,
			   *rc;

	foreach(lc, group)
	{
		DGBatch    *ib = lfirst(lc);

		rels = lappend(rels, ib->bound ? ib->bound : relation_open(ib->relid, NoLock));
		stale = stale || batch_stale(ib);
	}
	if (stale)
	{
		Snapshot	snapshot = RegisterSnapshot(GetLatestSnapshot());

		batch_group_snapshot = snapshot;
		PG_TRY();
		{
			batch_reload(b, rel);
			forboth(lc, group, rc, rels)
				batch_reload(lfirst(lc), lfirst(rc));
		}
		PG_FINALLY();
		{
			batch_group_snapshot = NULL;
		}
		PG_END_TRY();
		UnregisterSnapshot(snapshot);
	}
	batch_apply(b);
	forboth(lc, group, rc, rels)
	{
		DGBatch    *ib = lfirst(lc);
		Relation	irel = lfirst(rc);

		batch_apply(ib);
		if (!ib->bound)
			relation_close(irel, NoLock);
		batch_forget(ib);
	}
}

static void
batch_flush(DGBatch *b)
{
	RootKey		key = b->key;

	if (b->count)
	{
		Relation	bound = b->bound;
		Relation	rel = bound ? bound : relation_open(b->relid, NoLock);
		bool		apply = true;

		/*
		 * An index's edits follow its table's: the table's batch of the same
		 * command applies them with its own.
		 */
		if (!b->table)
		{
			RootKey		table_key = root_key_of(b->table_space, b->table_file, dg_relation_branch(rel));
			DGBatch    *tb = batch_find(&table_key);

			if (tb && tb->cid == b->cid && tb->subxid == b->subxid)
				batch_flush(tb);
			if (!batch_find(&key))
			{
				if (!bound)
					relation_close(rel, NoLock);
				dg_collect();
				return;
			}
		}

		if (apply && b->table && !b->bound)
		{
			batch_apply_group(b, rel);
			apply = false;
		}
		else if (apply)
		{
			/* Reload only if someone published roots since the state was loaded. */
			if (batch_stale(b))
				batch_reload(b, rel);
			batch_apply(b);
		}
		if (!bound)
			relation_close(rel, NoLock);
	}
	batch_forget(b);
	dg_collect();
}

/* Apply the pending edits to |key|'s roots, if any. */
static void
batch_flush_key(const RootKey *key)
{
	DGBatch    *b = batch_find(key);

	if (b)
		batch_flush(b);
}

static void
batch_flush_all(void)
{
	HASH_SEQ_STATUS status;
	DGBatch    *b;
	List	   *keys = NIL;
	ListCell   *lc;

	if (!batches)
		return;
	hash_seq_init(&status, batches);
	while ((b = hash_seq_search(&status)) != NULL)
	{
		RootKey    *key = palloc(sizeof(RootKey));

		*key = b->key;
		keys = lappend(keys, key);
	}
	foreach(lc, keys)
		batch_flush_key(lfirst(lc));
	list_free_deep(keys);
}

/* Forget the batches of an aborting subtransaction: nothing was applied. */
static void
batch_discard(SubTransactionId subxid)
{
	HASH_SEQ_STATUS status;
	DGBatch    *b;
	List	   *keys = NIL;
	ListCell   *lc;

	if (!batches)
		return;
	hash_seq_init(&status, batches);
	while ((b = hash_seq_search(&status)) != NULL)
		if (b->subxid == subxid)
		{
			RootKey    *key = palloc(sizeof(RootKey));

			*key = b->key;
			keys = lappend(keys, key);
		}
	foreach(lc, keys)
	{
		MemoryContext memory;

		b = batch_find(lfirst(lc));
		memory = b->memory;
		hash_search(batches, lfirst(lc), HASH_REMOVE, NULL);
		MemoryContextDelete(memory);
	}
	list_free_deep(keys);
}

/*
 * The batch of the current command's edits to |rel| on its branch, holding
 * the state they apply to. An older command's batch is applied first.
 */
DGBatch *
dg_batch(Relation rel, CommandId cid)
{
	RootKey		key = root_key(rel, dg_relation_branch(rel));
	DGBatch    *b;
	bool		found;
	MemoryContext old;

	register_callbacks();
	b = batch_find(&key);
	if (b && (b->cid != cid || b->subxid != GetCurrentSubTransactionId()))
	{
		batch_flush(b);
		b = NULL;
	}
	if (b)
		return b;
	if (!batches)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(RootKey);
		ctl.entrysize = sizeof(DGBatch);
		ctl.hcxt = TopTransactionContext;
		batches = hash_create("prolly pending edits", 16, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	b = hash_search(batches, &key, HASH_ENTER, &found);
	Assert(!found);
	b->relid = RelationGetRelid(rel);
	b->bound = dg_storage_bound(rel) ? rel : NULL;
	b->cid = cid;
	b->subxid = GetCurrentSubTransactionId();
	b->table = rel->rd_rel->relkind != RELKIND_INDEX;
	b->on_snapshot = on_snapshot(rel, SnapshotSelf);
	b->memory = AllocSetContextCreate(TopTransactionContext, "prolly pending edits", ALLOCSET_DEFAULT_SIZES);
	b->count = 0;
	b->entries = NULL;
	b->entry_count = b->entry_capacity = 0;
	old = MemoryContextSwitchTo(b->memory);
	if (b->table)
	{
		b->table_db = rel->rd_locator.dbOid;
		b->table_space = rel->rd_locator.spcOid;
		b->table_file = rel->rd_locator.relNumber;
	}
	else
	{
		Relation	table;
		RootKey		table_key = index_table_key(rel, dg_relation_branch(rel), &table);

		b->table_db = table ? table->rd_locator.dbOid : MyDatabaseId;
		b->table_space = table_key.space;
		b->table_file = table_key.file;
		if (table)
			relation_close(table, NoLock);
	}
	batch_stamp(b);
	b->state = batch_load(rel);
	if (b->table)
	{
		b->rows = batch_table(b->memory, "prolly pending rows", sizeof(PLHash), sizeof(PendingRow));
		b->handles = batch_table(b->memory, "prolly pending handles", sizeof(uint64), sizeof(PendingHandle));
		b->prefixes = NULL;
	}
	else
	{
		b->rows = b->handles = NULL;
		b->prefixes = batch_table(b->memory, "prolly pending prefixes", sizeof(uint64), sizeof(PendingPrefix));
	}
	MemoryContextSwitchTo(old);
	return b;
}

/*
 * A batch's trees are the latest while nothing has written its roots and no
 * write to its storage, versions or handles, has committed since they were
 * loaded.
 */
static void
batch_stamp(DGBatch *b)
{
	b->generation = roots_generation(&b->key);
	b->words[STORAGE_HANDLES] = pg_atomic_read_u64(storage_word(b->key.space, b->key.file, STORAGE_HANDLES));
	b->words[STORAGE_DELTA] = pg_atomic_read_u64(storage_word(b->key.space, b->key.file, STORAGE_DELTA));
}

static bool
batch_stale(DGBatch *b)
{
	/* A batch built on the transaction snapshot never goes stale. */
	if (b->on_snapshot)
		return false;
	if (b->generation != roots_generation(&b->key))
		return true;

	/* A commit that changed the storage, or one under way when it was loaded, has moved the word. */
	return pg_atomic_read_u64(storage_word(b->key.space, b->key.file, STORAGE_HANDLES)) != b->words[STORAGE_HANDLES] ||
		pg_atomic_read_u64(storage_word(b->key.space, b->key.file, STORAGE_DELTA)) != b->words[STORAGE_DELTA];
}

/* Whether the command's batch for |key| is adding the row version |handle|. */
static bool
batch_adds_handle(const RootKey *key, uint64 handle)
{
	DGBatch    *b = batch_find(key);
	PendingHandle *mapping;

	if (!b || !b->table)
		return false;
	mapping = hash_search(b->handles, &handle, HASH_FIND, NULL);
	return mapping && mapping->present;
}

/* The batch recording edits to |rel|, if the current command has one. */
DGBatch *
dg_batch_current(Relation rel)
{
	RootKey		key = root_key(rel, dg_relation_branch(rel));
	DGBatch    *b = batch_find(&key);

	return b && b->cid == GetCurrentCommandId(false) && b->subxid == GetCurrentSubTransactionId() ? b : NULL;
}

/* The current command's batch for |rel| while its trees are still the latest. */
DGBatch *
dg_batch_fresh(Relation rel)
{
	DGBatch    *b = dg_batch_current(rel);

	return b && !batch_stale(b) ? b : NULL;
}

/*
 * The batch for |rel| with trees that are the latest: a batch loaded before
 * another transaction published roots is applied first, so that what the
 * caller reads through the new one includes that transaction's rows.
 */
DGBatch *
dg_batch_latest(Relation rel, CommandId cid)
{
	DGBatch    *b = dg_batch(rel, cid);

	if (batch_stale(b))
	{
		batch_flush(b);
		b = dg_batch(rel, cid);
	}
	return b;
}

DGState *
dg_batch_state(DGBatch *b)
{
	return b->state;
}

/* Record one more occurrence of |row| under |handle|: content, handle and key. */
void
dg_batch_insert(DGBatch *b, PLSlice row, uint64 handle)
{
	PLHash		digest = dg_rowhash(row);
	MemoryContext old = MemoryContextSwitchTo(b->memory);
	PendingRow *pending;
	PendingHandle *mapping;
	bool		found;

	dg_version_mark_branch(b->key.branch);

	pending = hash_search(b->rows, &digest, HASH_ENTER, &found);
	if (!found)
	{
		pending->delta = 0;
		pending->row = slice_copy(row);
		pending->handles = NULL;
		pending->handle_count = 0;
	}
	pending->delta++;
	pending->handles = pending->handles ? repalloc(pending->handles, (pending->handle_count + 1) * sizeof(uint64)) : palloc(sizeof(uint64));
	pending->handles[pending->handle_count++] = handle;
	mapping = hash_search(b->handles, &handle, HASH_ENTER, NULL);
	mapping->digest = digest;
	mapping->present = true;
	b->count++;
	MemoryContextSwitchTo(old);
}

/* Record one occurrence of |row| under |handle| removed. */
void
dg_batch_remove(DGBatch *b, PLSlice row, uint64 handle)
{
	PLHash		digest = dg_rowhash(row);
	MemoryContext old = MemoryContextSwitchTo(b->memory);
	PendingRow *pending;
	PendingHandle *mapping;
	bool		found;

	dg_version_mark_branch(b->key.branch);

	pending = hash_search(b->rows, &digest, HASH_ENTER, &found);
	if (!found)
	{
		pending->delta = 0;
		pending->row = slice_copy(row);
		pending->handles = NULL;
		pending->handle_count = 0;
	}
	pending->delta--;
	for (int i = 0; i < pending->handle_count; i++)
		if (pending->handles[i] == handle)
			pending->handles[i] = pending->handles[--pending->handle_count];
	mapping = hash_search(b->handles, &handle, HASH_ENTER, &found);
	/* An insert cancelled before the batch was flushed never existed in the
	 * base state.  Do not record a deletion (or reserve a tombstone) for it. */
	if (found && mapping->present && !dg_handle_digest(b->state, handle, &digest))
		hash_search(b->handles, &handle, HASH_REMOVE, NULL);
	else
	{
		mapping->digest = digest;
		mapping->present = false;
	}
	b->count++;
	MemoryContextSwitchTo(old);
}

/* Record an index entry |key| for the row with |digest|, added (delta 1) or removed (-1). */
void
dg_batch_index(DGBatch *b, PLSlice key, size_t prefix, PLHash digest, int64 delta)
{
	MemoryContext old = MemoryContextSwitchTo(b->memory);
	PendingEntry *entry;
	PendingPrefix *group;
	uint64		hash = hash_bytes_extended(key.data, prefix, 0);
	bool		found;

	/* Entries are allocated one by one: the prefix groups point at them. */
	if (b->entry_count == b->entry_capacity)
	{
		b->entry_capacity = b->entry_capacity ? b->entry_capacity * 2 : 64;
		b->entries = b->entries ? repalloc(b->entries, b->entry_capacity * sizeof(PendingEntry *)) :
			palloc(b->entry_capacity * sizeof(PendingEntry *));
	}
	entry = palloc(sizeof(PendingEntry));
	b->entries[b->entry_count++] = entry;
	entry->key = slice_copy(key);
	entry->digest = digest;
	entry->delta = delta;
	group = hash_search(b->prefixes, &hash, HASH_ENTER, &found);
	if (!found)
		group->entries = NIL;
	group->entries = lappend(group->entries, entry);
	b->count++;
	MemoryContextSwitchTo(old);
}

/*
 * The digests of the index entries whose key starts with |prefix| that the
 * batch adds (into |added|) or removes (into |removed|).
 */
void
dg_batch_index_prefix(DGBatch *b, PLSlice prefix, List **added, List **removed)
{
	uint64		hash = hash_bytes_extended(prefix.data, prefix.len, 0);
	PendingPrefix *group = hash_search(b->prefixes, &hash, HASH_FIND, NULL);
	ListCell   *lc;

	if (!group)
		return;
	foreach(lc, group->entries)
	{
		PendingEntry *entry = lfirst(lc);
		PLHash	   *digest;

		if (entry->key.len < prefix.len || memcmp(entry->key.data, prefix.data, prefix.len))
			continue;
		digest = palloc(sizeof(PLHash));
		*digest = entry->digest;
		if (entry->delta > 0)
			*added = lappend(*added, digest);
		else
			*removed = lappend(*removed, digest);
	}
}

/* The digest |handle| maps to, as the batch's command sees it. */
bool
dg_batch_handle_digest(DGBatch *b, uint64 handle, PLHash *digest)
{
	PendingHandle *mapping = hash_search(b->handles, &handle, HASH_FIND, NULL);

	if (mapping)
	{
		*digest = mapping->digest;
		return mapping->present;
	}
	return dg_handle_digest(b->state, handle, digest);
}

/*
 * Whether the command's pending edits keep a new occurrence of |digest| from
 * taking the derived |handle|: an occurrence of the row already added, or
 * any edit naming the handle, as a row removed and re-added would.
 */
bool
dg_batch_blocks_handle(DGBatch *b, PLHash digest, uint64 handle)
{
	PendingRow *pending = hash_search(b->rows, &digest, HASH_FIND, NULL);

	if (pending && pending->delta > 0)
		return true;
	return hash_search(b->handles, &handle, HASH_FIND, NULL) != NULL;
}

/* The row with |digest|, as the batch's command sees it. */
bool
dg_batch_row(DGBatch *b, PLHash digest, PLSlice *row)
{
	PendingRow *pending = hash_search(b->rows, &digest, HASH_FIND, NULL);
	PLValue		value;

	if (pending)
	{
		*row = pending->row;
		return true;
	}
	if (!dg_get(b->state, (PLSlice) {digest.bytes, PL_HASH_SIZE}, &value) || value.bytes.len < 8)
		return false;
	*row = (PLSlice) {value.bytes.data + 8, value.bytes.len - 8};
	return true;
}

/* The handles the batch added for the row with |digest|. */
int
dg_batch_handles(DGBatch *b, PLHash digest, uint64 **handles)
{
	PendingRow *pending = hash_search(b->rows, &digest, HASH_FIND, NULL);

	*handles = pending ? pending->handles : NULL;
	return pending ? pending->handle_count : 0;
}

/*
 * The latest trees of |rel| without the current command's pending edits,
 * which the caller releases: a copy of its batch's state while nothing has
 * published roots since that was loaded, else freshly loaded.
 */
DGState *
dg_batch_load(Relation rel)
{
	DGBatch    *b = dg_batch_current(rel);
	DGState    *copy;

	if (!b || batch_stale(b))
		return batch_load(rel);
	copy = palloc0(sizeof(DGState));
	*copy = *b->state;
	copy->shared = true;
	memset(&copy->cleanup, 0, sizeof(copy->cleanup));
	return copy;
}

/* Apply |rel|'s pending edits now, as the end of a bulk insert does. */
void
dg_batch_finish(Relation rel)
{
	RootKey		key = root_key(rel, dg_relation_branch(rel));

	batch_flush_key(&key);
}
