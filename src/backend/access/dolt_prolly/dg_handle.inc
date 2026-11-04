/* ---------------------------------------------------------------------------
 * Row handles: derived from content, with a catalog delta for the rest
 * ---------------------------------------------------------------------------
 *
 * A handle names one row occurrence of a table on a branch; the executor
 * sees it as the row's TID. Most handles are derived: the first occurrence
 * of a row takes the leading DG_HANDLE_PREFIX_BITS of the row's digest, so
 * it is a pure function of the row, needs no storage, and is the same on
 * every branch and in every revision. Resolving a derived handle is a range
 * lookup in the content tree for the digests with that prefix; the row
 * whose occurrences exceed its allocated handles owns the derived one.
 *
 * Occurrences that cannot take the derived handle get one from the
 * storage's counter, above DG_DERIVED_HANDLES, and pg_dolt_handle maps those
 * to their digest: further occurrences of a row already present, rows whose
 * prefix another live row or another transaction's claim holds, and rows
 * whose derived handle was recently used, so that a lock record still
 * naming it cannot confuse a reader. Allocated handles are the only handle state
 * written per row, and a branch inherits none of them from its parent:
 * creating a branch copies nothing.
 *
 * The delta is read through a per-backend cache of a (storage, branch)
 * stream, keyed by the snapshot kind it was read under and validated by the
 * same visibility identity and no local writes to the storage's handles
 * since, or the same snapshot but for a later command when this transaction
 * wrote none of them since the delta was read. Across transactions, a delta
 * read under the latest snapshot while no commit to its storage was under way
 * stands while the storage's word does not move: every handle write touches
 * the word, and its commit moves it (storage_touch). States that do not read the
 * catalog, revisions and private rewrites, carry a private delta of their own.
 *
 * Nothing canonical depends on a handle: the content and index trees hold
 * rows and digests only.
 */

#define DG_HANDLE_PREFIX_BITS 40

typedef struct HandleMapping
{
	uint64		handle;			/* hash key */
	PLHash		digest;
} HandleMapping;

typedef struct HandleDigest
{
	PLHash		digest;			/* hash key */
	uint64	   *handles;
	int			count,
				capacity;
} HandleDigest;

typedef struct HandleDeltaKey
{
	RootKey		key;
	int			kind;			/* 0: MVCC snapshot; 1: the latest state */
	int			pad;
} HandleDeltaKey;

typedef struct HandleDelta HandleDelta;

struct HandleDelta
{
	HandleDeltaKey key;			/* hash key, for cached deltas */
	bool		loaded;
	Visibility	seen;
	uint64		local_version;	/* handle_local_version_of its storage, when read */
	bool		streamed;		/* read as handle_stream_build reads, and unchanged since */
	uint64		word;			/* the storage's word then */
	TransactionId xmax;			/* the snapshot read under, to tell what other snapshots see */
	uint32		xcnt;
	TransactionId *xip;
	MemoryContext memory;
	HTAB	   *by_handle;		/* HandleMapping */
	HTAB	   *by_digest;		/* HandleDigest */
	int			count;
};

/*
 * A storage's shared word: a generation in the high bits, moved by every
 * commit that changed the storage's handles, and its committers under way
 * in the low bits. A reader builds or keeps a cache of the storage's delta
 * only while no committer is under way and the generation stands.
 */
#define STORAGE_WORD_GENERATION	(UINT64CONST(1) << 16)
#define STORAGE_WORD_COMMITTERS	(STORAGE_WORD_GENERATION - 1)

/* Storages whose handles this transaction changed, whose readers must notice at commit. */
typedef struct StorageTouched
{
	Oid			db,
				space;
	RelFileNumber file;
	bool		counted;		/* counted as a committer in the storage's word */
} StorageTouched;

static StorageTouched *storage_touched;
static int	storage_touched_count,
			storage_touched_capacity;
static bool storage_committing;	/* past pre-commit: a new touch counts at once */

static HandleSlot *handle_slot_claim(Oid db, Oid space, RelFileNumber file);

static pg_atomic_uint64 *
storage_word(Oid space, RelFileNumber file)
{
	return &handle_slot_claim(MyDatabaseId, space, file)->commit_generation;
}

/* |touched|'s storage has a committer under way: this transaction. */
static void
storage_count_committer(StorageTouched *touched)
{
	pg_atomic_fetch_add_u64(storage_word(touched->space, touched->file), STORAGE_WORD_GENERATION + 1);
	touched->counted = true;
}

/* This transaction changed the handles of storage (|db|, |space|, |file|). */
static void
storage_touch(Oid db, Oid space, RelFileNumber file)
{
	MemoryContext old;

	for (int i = 0; i < storage_touched_count; i++)
		if (storage_touched[i].db == db && storage_touched[i].space == space && storage_touched[i].file == file)
			return;
	old = MemoryContextSwitchTo(TopTransactionContext);
	if (storage_touched_count == storage_touched_capacity)
	{
		storage_touched_capacity = storage_touched_capacity ? storage_touched_capacity * 2 : 8;
		storage_touched = storage_touched ? repalloc(storage_touched, storage_touched_capacity * sizeof(StorageTouched)) :
			palloc(storage_touched_capacity * sizeof(StorageTouched));
	}
	storage_touched[storage_touched_count].db = db;
	storage_touched[storage_touched_count].space = space;
	storage_touched[storage_touched_count].file = file;
	storage_touched[storage_touched_count].counted = false;
	storage_touched_count++;
	MemoryContextSwitchTo(old);

	/* Claim the storage's slot now: the commit must find its word. */
	(void) storage_word(space, file);
	if (storage_committing)
		storage_count_committer(&storage_touched[storage_touched_count - 1]);
}

/*
 * Before the commit becomes visible, count it as a committer of every storage
 * whose handles it changed: a reader then neither builds a cache nor keeps
 * one until the commit is visible and counted out.
 */
static void
storage_pre_commit(void)
{
	for (int i = 0; i < storage_touched_count; i++)
		if (!storage_touched[i].counted)
			storage_count_committer(&storage_touched[i]);
	storage_committing = true;
}

/*
 * At transaction end: count this transaction out of the storages it counted
 * itself a committer of, which moves their generation again.
 */
static void
storage_xact_end(bool committed)
{
	/* The slots were claimed at the touch: finding them reads no catalogs. */
	shared_handles();
	for (int i = 0; i < storage_touched_count; i++)
	{
		StorageTouched *touched = &storage_touched[i];
		HandleSlot *slot = handle_slot_find(touched->db, touched->space, touched->file);

		if (slot && touched->counted)
			pg_atomic_fetch_add_u64(&slot->commit_generation, STORAGE_WORD_GENERATION - 1);
		else if (slot && committed)
			pg_atomic_fetch_add_u64(&slot->commit_generation, STORAGE_WORD_GENERATION);
	}
	storage_touched = NULL;
	storage_touched_count = storage_touched_capacity = 0;
	storage_committing = false;
}

static HTAB *handle_deltas;

/*
 * What this backend wrote to the catalog, as delta validity sees it: an epoch
 * that moves when a transaction or subtransaction ends, which may discard or
 * reveal writes to any storage, and a version per storage that moves with
 * each write to that storage's handles. A write to one table leaves the
 * deltas of the others valid.
 */
static uint64 handle_local_epoch = 1;

typedef struct HandleVersion
{
	Oid			space;			/* hash key */
	RelFileNumber file;
	uint64		version;
	CommandId	last_cid;		/* the command of this transaction's last write */
} HandleVersion;

static HTAB *handle_versions;

static void handle_delta_tables(HandleDelta *delta);
static HandleDelta *private_delta(void);
static void handle_delta_note(HandleDelta *delta, uint64 handle, PLHash digest, bool present);
static void handle_delta_load(HandleDelta *delta, Snapshot snapshot);
static uint64 handle_allocate(Relation rel, DGState *s);

static inline uint64
handle_derived(PLHash digest)
{
	return pl_read64(digest.bytes) >> (64 - DG_HANDLE_PREFIX_BITS);
}

static inline bool
handle_is_derived(uint64 handle)
{
	return handle < DG_DERIVED_HANDLES;
}

static inline bool
handle_is_synthetic(uint64 handle)
{
	return handle >= DG_SYNTHETIC_HANDLES;
}

/* The digests deriving |handle|: those in [lower, upper), or from lower on. */
static bool
handle_prefix_bounds(uint64 handle, PLHash *lower, PLHash *upper)
{
	memset(lower->bytes, 0, PL_HASH_SIZE);
	pl_write64(lower->bytes, handle << (64 - DG_HANDLE_PREFIX_BITS));
	if (handle + 1 >= DG_DERIVED_HANDLES)
		return false;
	memset(upper->bytes, 0, PL_HASH_SIZE);
	pl_write64(upper->bytes, (handle + 1) << (64 - DG_HANDLE_PREFIX_BITS));
	return true;
}

/* The digest range of the |count| derived handles from |first|; false if unbounded above. */
bool
dg_handle_range(uint64 first, uint64 count, PLHash *lower, PLHash *upper)
{
	memset(lower->bytes, 0, PL_HASH_SIZE);
	pl_write64(lower->bytes, first << (64 - DG_HANDLE_PREFIX_BITS));
	if (first + count >= DG_DERIVED_HANDLES)
		return false;
	memset(upper->bytes, 0, PL_HASH_SIZE);
	pl_write64(upper->bytes, (first + count) << (64 - DG_HANDLE_PREFIX_BITS));
	return true;
}

static void
handle_delta_tables(HandleDelta *delta)
{
	HASHCTL		ctl = {0};

	ctl.keysize = sizeof(uint64);
	ctl.entrysize = sizeof(HandleMapping);
	ctl.hcxt = delta->memory;
	delta->by_handle = hash_create("prolly handle delta by handle", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	ctl.keysize = sizeof(PLHash);
	ctl.entrysize = sizeof(HandleDigest);
	delta->by_digest = hash_create("prolly handle delta by digest", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	delta->count = 0;
}

/* Record, or forget, that allocated |handle| names an occurrence of |digest|. */
static void
handle_delta_note(HandleDelta *delta, uint64 handle, PLHash digest, bool present)
{
	HandleMapping *mapping;
	HandleDigest *entry;
	bool		found;

	if (present)
	{
		mapping = hash_search(delta->by_handle, &handle, HASH_ENTER, &found);
		if (found)
			return;
		mapping->digest = digest;
		entry = hash_search(delta->by_digest, &digest, HASH_ENTER, &found);
		if (!found)
		{
			entry->handles = NULL;
			entry->count = entry->capacity = 0;
		}
		if (entry->count == entry->capacity)
		{
			MemoryContext old = MemoryContextSwitchTo(delta->memory);

			entry->capacity = entry->capacity ? entry->capacity * 2 : 2;
			entry->handles = entry->handles ? repalloc(entry->handles, entry->capacity * sizeof(uint64)) :
				palloc(entry->capacity * sizeof(uint64));
			MemoryContextSwitchTo(old);
		}
		entry->handles[entry->count++] = handle;
		delta->count++;
		return;
	}
	mapping = hash_search(delta->by_handle, &handle, HASH_REMOVE, NULL);
	if (!mapping)
		return;
	entry = hash_search(delta->by_digest, &digest, HASH_FIND, NULL);
	if (entry)
	{
		for (int i = 0; i < entry->count; i++)
			if (entry->handles[i] == handle)
				entry->handles[i] = entry->handles[--entry->count];
		if (!entry->count)
			hash_search(delta->by_digest, &digest, HASH_REMOVE, NULL);
	}
	delta->count--;
}

/* Fill |delta| from the catalog rows of its key that |snapshot| sees. */
static void
handle_delta_load(HandleDelta *delta, Snapshot snapshot)
{
	Relation	catalog = table_open(DoltHandleRelationId, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(catalog);
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		registered = snapshot && IsMVCCSnapshot(snapshot);
	bool		pushed = false;

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (registered)
		snapshot = RegisterSnapshot(snapshot);
	else
		snapshot = SnapshotSelf;
	MemoryContextReset(delta->memory);
	handle_delta_tables(delta);
	ScanKeyInit(&keys[0], Anum_pg_dolt_handle_hdlspace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(delta->key.key.space));
	ScanKeyInit(&keys[1], Anum_pg_dolt_handle_hdlfile, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(delta->key.key.file));
	ScanKeyInit(&keys[2], Anum_pg_dolt_handle_hdlbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(delta->key.key.branch));
	scan = systable_beginscan(catalog, DoltHandleKeyIndexId, true, snapshot, 3, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_dolt_handle form = (Form_pg_dolt_handle) GETSTRUCT(tuple);
		PLHash		digest;
		bool		isnull;
		bytea	   *bytes = DatumGetByteaPP(heap_getattr(tuple, Anum_pg_dolt_handle_hdldigest, desc, &isnull));

		if (VARSIZE_ANY_EXHDR(bytes) != PL_HASH_SIZE)
			elog(ERROR, "invalid prolly handle digest");
		memcpy(digest.bytes, VARDATA_ANY(bytes), PL_HASH_SIZE);
		handle_delta_note(delta, (uint64) form->hdlhandle, digest, true);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	if (registered)
		UnregisterSnapshot(snapshot);
	if (pushed)
		PopActiveSnapshot();
	delta->loaded = true;
}

static HandleVersion *
handle_version_entry(Oid space, RelFileNumber file, bool enter)
{
	struct
	{
		Oid			space;
		RelFileNumber file;
	}			key = {space, file};

	if (!handle_versions)
	{
		HASHCTL		ctl = {0};

		if (!enter)
			return NULL;
		ctl.keysize = sizeof(key);
		ctl.entrysize = sizeof(HandleVersion);
		ctl.hcxt = TopMemoryContext;
		handle_versions = hash_create("prolly handle versions", 32, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	return hash_search(handle_versions, &key, enter ? HASH_ENTER : HASH_FIND, NULL);
}

/* The version a delta of |key|'s storage must have been read at to stand. */
static uint64
handle_local_version_of(const RootKey *key)
{
	HandleVersion *entry = handle_version_entry(key->space, key->file, false);

	return (handle_local_epoch << 32) + (entry ? entry->version : 0);
}

/* This backend changed |key|'s storage's handles in command |cid|. */
static void
handle_local_write(const RootKey *key, CommandId cid)
{
	HandleVersion *entry = handle_version_entry(key->space, key->file, true);

	/*
	 * Readers of other backends notice the commit by the storage's word, and
	 * this backend's deltas of the storage, which note the write, no longer
	 * hold only what a snapshot read.
	 */
	storage_touch(MyDatabaseId, key->space, key->file);
	if (handle_deltas)
	{
		HASH_SEQ_STATUS status;
		HandleDelta *delta;

		hash_seq_init(&status, handle_deltas);
		while ((delta = hash_seq_search(&status)) != NULL)
			if (delta->key.key.space == key->space && delta->key.key.file == key->file)
				delta->streamed = false;
	}

	if (entry->space != key->space || entry->file != key->file || entry->version == 0)
	{
		entry->space = key->space;
		entry->file = key->file;
		entry->version = 0;
	}
	entry->version++;
	entry->last_cid = cid;
}

/* A transaction or subtransaction ended: its writes may have gone or become visible. */
static void
handle_local_invalidate(void)
{
	handle_local_epoch++;
}

/*
 * Whether a delta read under |built| sees the same rows as a load under
 * |now|: the same snapshot but for a later command, in the same epoch, when
 * this transaction wrote none of the storage's handles in or after the
 * command the delta was read in. A later command sees more of the
 * transaction's own writes and nothing else.
 */
static bool
handle_same_view(const HandleDelta *delta, const Visibility *now, const RootKey *key)
{
	Visibility	a = delta->seen,
				b = *now;
	HandleVersion *entry;

	if (a.overflow || b.overflow || a.cid == InvalidCommandId || b.cid == InvalidCommandId ||
		b.cid < a.cid || (delta->local_version >> 32) != handle_local_epoch)
		return false;
	a.cid = b.cid = 0;
	if (memcmp(&a, &b, sizeof(a)) != 0)
		return false;
	entry = handle_version_entry(key->space, key->file, false);
	return !entry || entry->version == 0 || entry->last_cid < delta->seen.cid;
}

/* Whether this transaction changed |key|'s storage's handles. */
static bool
handle_storage_touched(const RootKey *key)
{
	for (int i = 0; i < storage_touched_count; i++)
		if (storage_touched[i].space == key->space && storage_touched[i].file == key->file)
			return true;
	return false;
}

/*
 * Read |key|'s delta under the latest snapshot: false when a commit to the storage is under way, or this
 * transaction changed it, so that the delta would hold uncommitted rows.
 */
static bool
handle_stream_build(HandleDelta *delta, const RootKey *key)
{
	uint64		word = pg_atomic_read_u64(storage_word(key->space, key->file));
	Snapshot	latest;
	uint32		n;

	if ((word & STORAGE_WORD_COMMITTERS) || handle_storage_touched(key))
		return false;
	pg_memory_barrier();
	latest = RegisterSnapshot(GetLatestSnapshot());
	if (latest->suboverflowed)
	{
		UnregisterSnapshot(latest);
		return false;
	}
	handle_delta_load(delta, latest);
	n = latest->xcnt + latest->subxcnt;
	delta->xip = MemoryContextAlloc(delta->memory, Max(n, 1) * sizeof(TransactionId));
	memcpy(delta->xip, latest->xip, latest->xcnt * sizeof(TransactionId));
	memcpy(delta->xip + latest->xcnt, latest->subxip, latest->subxcnt * sizeof(TransactionId));
	delta->xcnt = n;
	delta->xmax = latest->xmax;
	UnregisterSnapshot(latest);
	delta->word = word;
	delta->streamed = true;
	return true;
}

/*
 * Whether |delta|, as handle_stream_build read it, holds what |snapshot|
 * sees: no handle of the storage committed since, and this transaction
 * changed none; and an MVCC snapshot sees every transaction the read saw
 * committed. The latest state sees them all.
 */
static bool
handle_stream_serves(const HandleDelta *delta, const RootKey *key, Snapshot snapshot)
{
	if (!delta->loaded || !delta->streamed || handle_storage_touched(key) ||
		pg_atomic_read_u64(storage_word(key->space, key->file)) != delta->word)
		return false;
	if (!snapshot || !IsMVCCSnapshot(snapshot))
		return true;
	if (snapshot->snapshot_type != SNAPSHOT_MVCC || snapshot->suboverflowed ||
		TransactionIdPrecedes(snapshot->xmax, delta->xmax))
		return false;
	for (uint32 i = 0; i < snapshot->xcnt + snapshot->subxcnt; i++)
	{
		TransactionId xid = i < snapshot->xcnt ? snapshot->xip[i] : snapshot->subxip[i - snapshot->xcnt];
		bool		running = false;

		if (!TransactionIdPrecedes(xid, delta->xmax))
			continue;
		for (uint32 j = 0; j < delta->xcnt && !running; j++)
			running = TransactionIdEquals(delta->xip[j], xid);
		if (!running)
			return false;
	}
	return true;
}

/* The cached delta of |key| as |snapshot| sees it, reloaded if anything moved. */
static struct HandleDelta *
handle_delta_get(const RootKey *key, Snapshot snapshot)
{
	HandleDeltaKey cache_key = {0};
	HandleDelta *delta;
	Visibility	seen;
	bool		found;

	if (!handle_deltas)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(HandleDeltaKey);
		ctl.entrysize = sizeof(HandleDelta);
		ctl.hcxt = CacheMemoryContext;
		handle_deltas = hash_create("prolly handle deltas", 32, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	cache_key.key = *key;
	cache_key.kind = snapshot && IsMVCCSnapshot(snapshot) ? 0 : 1;
	delta = hash_search(handle_deltas, &cache_key, HASH_ENTER, &found);
	if (!found)
	{
		memset(((char *) delta) + sizeof(HandleDeltaKey), 0, sizeof(*delta) - sizeof(HandleDeltaKey));
		delta->memory = AllocSetContextCreate(CacheMemoryContext, "prolly handle delta", ALLOCSET_SMALL_SIZES);
	}
	if (handle_stream_serves(delta, key, snapshot))
		return delta;
	visibility_capture(snapshot, &seen);
	if (delta->loaded &&
		((delta->local_version == handle_local_version_of(key) && visibility_equal(&delta->seen, &seen)) ||
		 handle_same_view(delta, &seen, key)))
		return delta;
	if (!handle_stream_build(delta, key) || !handle_stream_serves(delta, key, snapshot))
	{
		handle_delta_load(delta, snapshot);
		delta->streamed = false;
	}
	delta->seen = seen;
	delta->local_version = handle_local_version_of(key);
	return delta;
}

/* A state's delta: its catalog view, or a private, empty one made on demand. */
static HandleDelta *
state_delta(DGState *s)
{
	if (!s->delta)
	{
		s->delta = private_delta();
		s->delta_private = true;
	}
	if (s->delta_private && !s->nextid)
		s->nextid = DG_DERIVED_HANDLES + 1;
	return s->delta;
}

static HandleDelta *
private_delta(void)
{
	HandleDelta *delta = palloc0(sizeof(*delta));

	delta->memory = AllocSetContextCreate(CurrentMemoryContext, "prolly private handle delta", ALLOCSET_SMALL_SIZES);
	handle_delta_tables(delta);
	delta->loaded = true;
	return delta;
}

/* The allocated handles of |digest| in |s|'s view; the array belongs to the delta. */
static int
delta_handles(DGState *s, PLHash digest, uint64 **handles)
{
	HandleDelta *delta = state_delta(s);
	HandleDigest *entry;

	*handles = NULL;
	if (!delta->count)
		return 0;
	entry = hash_search(delta->by_digest, &digest, HASH_FIND, NULL);
	if (!entry)
		return 0;
	*handles = entry->handles;
	return entry->count;
}

/*
 * Occurrences of |digest| beyond those the delta names, |count| of them,
 * numbered for this state alone: a private state's duplicates, or those of a
 * branch whose storage has no roots of its own yet. The numbers lie above
 * every allocated handle, so they can mean nothing else.
 */
static int
extra_handles(DGState *s, PLHash digest, int64 count, uint64 **handles)
{
	HandleDigest *entry;

	*handles = NULL;
	if (count <= 0 && !s->extra)
		return 0;
	if (!s->extra)
	{
		s->extra = private_delta();
		s->extra->count = 0;
	}
	entry = hash_search(s->extra->by_digest, &digest, HASH_FIND, NULL);
	while ((entry ? entry->count : 0) < count)
	{
		uint64		handle = DG_SYNTHETIC_HANDLES + (uint64) s->extra->count;

		handle_delta_note(s->extra, handle, digest, true);
		entry = hash_search(s->extra->by_digest, &digest, HASH_FIND, NULL);
	}
	if (!entry)
		return 0;
	*handles = entry->handles;
	return entry->count;
}


static PLStatus
collect_prefix_row(void *arg, PLSlice key, PLSlice value)
{
	PrefixRows *rows = arg;

	if (rows->count == lengthof(rows->digests))
		return PL_STOP;
	memcpy(rows->digests[rows->count].bytes, key.data, PL_HASH_SIZE);
	rows->counts[rows->count] = value.len >= 8 ? (int64) pl_read64(value.data) : 0;
	rows->count++;
	return PL_OK;
}

/* The rows of |tree| whose digests derive |handle|: rarely more than one. */
static PrefixRows
prefix_rows(const PLTree *tree, uint64 handle)
{
	PrefixRows	rows = {0};
	PLHash		lower,
				upper;
	bool		bounded = handle_prefix_bounds(handle, &lower, &upper);
	PLSlice		lo = {lower.bytes, PL_HASH_SIZE},
				hi = {upper.bytes, PL_HASH_SIZE};
	PLStatus	status = pl_scan(tree, &lo, bounded ? &hi : NULL, collect_prefix_row, &rows);

	if (status != PL_STOP)
		dg_check(status);
	return rows;
}

/* The digest row |handle| names in |s|, if the row is live there. */
bool
dg_handle_digest(DGState *s, uint64 handle, PLHash *digest)
{
	HandleMapping *mapping;
	PrefixRows	rows;

	if (handle_is_synthetic(handle))
	{
		if (!s->extra)
			return false;
		mapping = hash_search(s->extra->by_handle, &handle, HASH_FIND, NULL);
		if (!mapping)
			return false;
		*digest = mapping->digest;
		return true;
	}
	if (!handle_is_derived(handle))
	{
		HandleDelta *delta = state_delta(s);

		if (!delta->count)
			return false;
		mapping = hash_search(delta->by_handle, &handle, HASH_FIND, NULL);
		if (!mapping)
			return false;
		*digest = mapping->digest;
		return true;
	}
	rows = prefix_rows(&s->content, handle);
	for (int i = 0; i < rows.count; i++)
	{
		uint64	   *allocated;
		int			n = delta_handles(s, rows.digests[i], &allocated);

		if (rows.counts[i] - n >= 1)
		{
			*digest = rows.digests[i];
			return true;
		}
	}
	return false;
}

/*
 * The handles of row |digest|'s occurrences in |s|, |count| of them if the
 * caller knows, into |*handles|, a buffer of |*capacity| grown as needed.
 * The derived handle comes first, for the occurrence no allocated handle
 * names; a private state allocates for duplicates it meets.
 */
int
dg_handles_of(DGState *s, PLHash digest, int64 count, uint64 **handles, int *capacity)
{
	uint64	   *allocated,
			   *extras;
	int			n,
				e,
				derived,
				total;

	if (count < 0)
	{
		PLValue		value;

		dg_check(pl_get(&s->content, (PLSlice) {digest.bytes, PL_HASH_SIZE}, &value));
		count = value.present ? (int64) pl_read64(value.bytes.data) : 0;
	}
	n = delta_handles(s, digest, &allocated);
	e = extra_handles(s, digest, count - n - 1, &extras);
	derived = (int) Max(count - n - e, 0);
	total = derived + n + e;
	if (*capacity < total)
	{
		*capacity = Max(total, 8);
		*handles = *handles ? repalloc(*handles, *capacity * sizeof(uint64)) : palloc(*capacity * sizeof(uint64));
	}
	for (int i = 0; i < derived; i++)
		(*handles)[i] = handle_derived(digest);
	for (int i = 0; i < n; i++)
		(*handles)[derived + i] = allocated[i];
	for (int i = 0; i < e; i++)
		(*handles)[derived + n + i] = extras[i];
	return total;
}

/* The number of row occurrences |s| holds: distinct rows plus duplicates. */
double
dg_handle_count(DGState *s)
{
	return (double) pl_count(&s->content) + (s->delta ? s->delta->count : 0);
}

/*
 * Claims on derived handles. A transaction that gives a new row its derived
 * handle claims the handle first, so that no other transaction gives a
 * different row with the same prefix, or an identical row, the same handle.
 * A claim must outlast its transaction: another writer may have loaded its
 * state before the claimant committed, and then cannot see the claimant's
 * row. It may be forgotten once the claimant precedes the xmin of every
 * running transaction that may write rows, since every state loaded since
 * includes its rows.
 *
 * Claims are a fixed array of transaction IDs, found by hashing (storage,
 * branch, handle). Handles that share a slot share its claim: a transaction
 * may take any handle of a slot it holds, and another transaction's live
 * claim makes a new row take an allocated handle, which costs a catalog row
 * but nothing else.
 */
#define DG_HANDLE_CLAIM_SLOTS (1 << 20)

typedef struct HandleClaims
{
	pg_atomic_uint32 slots[DG_HANDLE_CLAIM_SLOTS];
} HandleClaims;

static HandleClaims *handle_claims;

/* The horizon of the current command: claims made before it are forgotten. */
static TransactionId claim_horizon;
static LocalTransactionId claim_horizon_lxid;
static CommandId claim_horizon_cid;

static void
handle_claims_init(void *ptr)
{
	HandleClaims *claims = ptr;

	for (int i = 0; i < DG_HANDLE_CLAIM_SLOTS; i++)
		pg_atomic_init_u32(&claims->slots[i], InvalidTransactionId);
}

/*
 * The oldest xmin of any running transaction that may write rows, computed
 * once per command: an older value only keeps claims longer. Autovacuum
 * workers write no rows, and an ANALYZE of a large table holds its snapshot
 * for as long as it reads the table, which would keep every claim made
 * meanwhile, so that new rows sharing a claim slot take allocated handles.
 */
static TransactionId
handle_claim_horizon(void)
{
	if (claim_horizon_lxid != MyProc->vxid.lxid || claim_horizon_cid != GetCurrentCommandId(false))
	{
		claim_horizon = GetOldestXminIgnoring(PROC_IS_AUTOVACUUM);
		claim_horizon_lxid = MyProc->vxid.lxid;
		claim_horizon_cid = GetCurrentCommandId(false);
	}
	return claim_horizon;
}

/* Claim derived |handle| of |rel| on |branch|; false if another transaction holds it. */
static bool
handle_claim(Relation rel, const char *branch, uint64 handle)
{
	struct
	{
		Oid			db,
					space;
		RelFileNumber file;
		uint32		branch;
		uint64		handle;
	}			key;
	TransactionId me = GetTopTransactionId();
	pg_atomic_uint32 *slot;
	uint32		owner;
	bool		found;

	if (!handle_claims)
		handle_claims = GetNamedDSMSegment("dolt derived handle claims", sizeof(HandleClaims),
										   handle_claims_init, &found);

	/* The key is hashed as bytes: its padding must be zero. */
	memset(&key, 0, sizeof(key));
	key.db = rel->rd_locator.dbOid;
	key.space = rel->rd_locator.spcOid;
	key.file = rel->rd_locator.relNumber;
	key.branch = hash_bytes((const unsigned char *) branch, strlen(branch));
	key.handle = handle;
	slot = &handle_claims->slots[hash_bytes_extended((const unsigned char *) &key, sizeof(key), 0) % DG_HANDLE_CLAIM_SLOTS];
	owner = pg_atomic_read_u32(slot);
	for (;;)
	{
		if (owner == me)
			return true;
		if (TransactionIdIsValid(owner) && !TransactionIdPrecedes(owner, handle_claim_horizon()))
			return false;
		if (pg_atomic_compare_exchange_u32(slot, &owner, me))
			return true;
	}
}

/*
 * The handle for a new occurrence of |digest| in |s|, recorded through |b|
 * if the command batches its edits: derived when no live row holds its
 * prefix, nothing of this command names it, no lock record still names it,
 * and no other transaction's claim covers it; else allocated.
 *
 * Transactions cannot see one another's uncommitted rows, nor a row
 * committed after their state was loaded: the claim keeps two of them from
 * deriving one handle for different rows with the same prefix, or for
 * identical rows. A unique index does not, since rows that share a prefix
 * rarely share a key.
 */
uint64
dg_handle_choose(Relation rel, DGState *s, DGBatch *b, PLHash digest)
{
	uint64		handle = handle_derived(digest);
	bool		taken = b && dg_batch_blocks_handle(b, digest, handle);
	int			status = DG_RECORD_NONE;

	if (!taken)
		taken = prefix_rows(&s->content, handle).count > 0;
	if (!taken && !s->delta_private && handle_records_exist(rel->rd_locator.dbOid, rel->rd_locator.spcOid, rel->rd_locator.relNumber))
	{
		status = dg_record_status(&rel->rd_locator, dg_relation_branch(rel), (int64) handle);
		taken = status == DG_RECORD_LIVE;
	}
	if (!taken && !s->delta_private)
		taken = !handle_claim(rel, dg_relation_branch(rel), handle);
	if (taken)
		return handle_allocate(rel, s);

	/*
	 * Only now is the handle the row's: renewing a dead record of an old row
	 * for a row that then took an allocated handle would leave a live record
	 * naming the derived handle, and every later occurrence would allocate.
	 */
	if (status == DG_RECORD_DEAD)
		dg_record_renew(&rel->rd_locator, dg_relation_branch(rel), (int64) handle);
	return handle;
}

/* Give a private state's occurrence of |digest| the handle |handle|, or take it away. */
void
dg_handle_note(DGState *s, uint64 handle, PLHash digest, bool present)
{
	if (handle_is_derived(handle))
		return;
	handle_delta_note(state_delta(s), handle, digest, present);
}

/* ---- The catalog ---- */

static IndexInfo *handle_index_info[2];

static void
handle_catalog_open(Relation *catalog, Relation *indexes)
{
	*catalog = table_open(DoltHandleRelationId, RowExclusiveLock);
	indexes[0] = index_open(DoltHandleKeyIndexId, RowExclusiveLock);
	indexes[1] = index_open(DoltHandleDigestIndexId, RowExclusiveLock);
	for (int i = 0; i < 2; i++)
		if (!handle_index_info[i])
		{
			MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);

			handle_index_info[i] = BuildIndexInfo(indexes[i]);
			MemoryContextSwitchTo(old);
		}
}

static void
handle_catalog_close(Relation catalog, Relation *indexes)
{
	index_close(indexes[1], RowExclusiveLock);
	index_close(indexes[0], RowExclusiveLock);
	table_close(catalog, RowExclusiveLock);
}

static void
handle_catalog_insert(Relation catalog, Relation *indexes, const RootKey *key, uint64 handle, PLHash digest, CommandId cid)
{
	Datum		values[Natts_pg_dolt_handle];
	bool		nulls[Natts_pg_dolt_handle] = {false};
	bytea	   *bytes = palloc(VARHDRSZ + PL_HASH_SIZE);
	HeapTuple	tuple;
	TupleTableSlot *slot;
	EState	   *estate;

	SET_VARSIZE(bytes, VARHDRSZ + PL_HASH_SIZE);
	memcpy(VARDATA(bytes), digest.bytes, PL_HASH_SIZE);
	values[Anum_pg_dolt_handle_hdlspace - 1] = ObjectIdGetDatum(key->space);
	values[Anum_pg_dolt_handle_hdlfile - 1] = ObjectIdGetDatum(key->file);
	values[Anum_pg_dolt_handle_hdlhandle - 1] = Int64GetDatum((int64) handle);
	values[Anum_pg_dolt_handle_hdlbranch - 1] = CStringGetTextDatum(key->branch);
	values[Anum_pg_dolt_handle_hdldigest - 1] = PointerGetDatum(bytes);
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	heap_insert(catalog, tuple, cid, 0, NULL);
	slot = MakeSingleTupleTableSlot(RelationGetDescr(catalog), &TTSOpsHeapTuple);
	estate = CreateExecutorState();
	ExecStoreHeapTuple(tuple, slot, false);
	for (int i = 0; i < 2; i++)
	{
		Datum		ivalues[INDEX_MAX_KEYS];
		bool		inulls[INDEX_MAX_KEYS];

		FormIndexDatum(handle_index_info[i], slot, estate, ivalues, inulls);
		index_insert(indexes[i], ivalues, inulls, &tuple->t_self, catalog, UNIQUE_CHECK_NO, false, handle_index_info[i]);
	}
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	heap_freetuple(tuple);
	pfree(bytes);
}

/* Delete the row naming |handle|, if this transaction can see one. */
static void
handle_catalog_delete(Relation catalog, const RootKey *key, uint64 handle, CommandId cid)
{
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	tuple;

	ScanKeyInit(&keys[0], Anum_pg_dolt_handle_hdlspace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->space));
	ScanKeyInit(&keys[1], Anum_pg_dolt_handle_hdlfile, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->file));
	ScanKeyInit(&keys[2], Anum_pg_dolt_handle_hdlbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(key->branch));
	ScanKeyInit(&keys[3], Anum_pg_dolt_handle_hdlhandle, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64) handle));
	scan = systable_beginscan(catalog, DoltHandleKeyIndexId, true, SnapshotSelf, 4, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		TM_FailureData failure;
		TM_Result	result = heap_delete(catalog, &tuple->t_self, cid, InvalidSnapshot, true, &failure, false);

		if (result != TM_Ok && result != TM_Deleted && result != TM_Updated && result != TM_SelfModified)
			elog(ERROR, "could not delete prolly handle row");
	}
	systable_endscan(scan);
}

/*
 * Write a command's allocated handle changes: |mappings| name handles added
 * (present) or removed. Derived handles need no row.
 */
static void
handle_delta_write(const RootKey *key, HTAB *mappings, CommandId cid)
{
	HASH_SEQ_STATUS status;
	PendingHandle *mapping;
	Relation	catalog,
				indexes[2];
	bool		opened = false,
				pushed = false;

	hash_seq_init(&status, mappings);
	while ((mapping = hash_seq_search(&status)) != NULL)
	{
		if (handle_is_derived(mapping->handle))
			continue;
		if (!opened)
		{
			if (!ActiveSnapshotSet())
			{
				PushActiveSnapshot(GetTransactionSnapshot());
				pushed = true;
			}
			handle_catalog_open(&catalog, indexes);
			opened = true;
		}
		if (mapping->present)
			handle_catalog_insert(catalog, indexes, key, mapping->handle, mapping->digest, cid);
		else
			handle_catalog_delete(catalog, key, mapping->handle, cid);
	}
	if (opened)
	{
		handle_catalog_close(catalog, indexes);
		if (pushed)
			PopActiveSnapshot();
		handle_local_write(key, cid);
	}
}

/* One allocated handle change outside a batch: a history install's. */
static void
handle_delta_edit(DGState *s, const RootKey *key, uint64 handle, PLHash digest, bool present, CommandId cid)
{
	Relation	catalog,
				indexes[2];

	if (handle_is_derived(handle))
		return;
	if (s->delta_private)
	{
		handle_delta_note(state_delta(s), handle, digest, present);
		return;
	}
	handle_catalog_open(&catalog, indexes);
	if (present)
		handle_catalog_insert(catalog, indexes, key, handle, digest, cid);
	else
		handle_catalog_delete(catalog, key, handle, cid);
	handle_catalog_close(catalog, indexes);
	handle_local_write(key, cid);

	/* The state keeps using its cached delta: let it see the edit too. */
	handle_delta_note(state_delta(s), handle, digest, present);
}

/*
 * The largest allocated handle any row of |space|/|file| has on any branch,
 * whatever its visibility: the counter must pass them all.
 */
static uint64
handle_delta_max(Oid space, RelFileNumber file)
{
	Relation	catalog = table_open(DoltHandleRelationId, AccessShareLock);
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	uint64		max = 0;

	ScanKeyInit(&keys[0], Anum_pg_dolt_handle_hdlspace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(space));
	ScanKeyInit(&keys[1], Anum_pg_dolt_handle_hdlfile, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(file));
	scan = systable_beginscan(catalog, DoltHandleKeyIndexId, true, SnapshotAny, 2, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_dolt_handle form = (Form_pg_dolt_handle) GETSTRUCT(tuple);

		max = Max(max, (uint64) form->hdlhandle);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return max;
}

/* Whether an allocated |handle| of |key| was inserted by a committed transaction other than this one. */
static bool
handle_delta_committed(const RootKey *key, uint64 handle)
{
	Relation	catalog = table_open(DoltHandleRelationId, AccessShareLock);
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		committed = false;

	ScanKeyInit(&keys[0], Anum_pg_dolt_handle_hdlspace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->space));
	ScanKeyInit(&keys[1], Anum_pg_dolt_handle_hdlfile, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->file));
	ScanKeyInit(&keys[2], Anum_pg_dolt_handle_hdlbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(key->branch));
	ScanKeyInit(&keys[3], Anum_pg_dolt_handle_hdlhandle, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64) handle));
	scan = systable_beginscan(catalog, DoltHandleKeyIndexId, true, SnapshotAny, 4, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		TransactionId xmin = HeapTupleHeaderGetXmin(tuple->t_data);

		if (!TransactionIdIsCurrentTransactionId(xmin) && TransactionIdDidCommit(xmin))
			committed = true;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return committed;
}

/* The digest an allocated |handle| of |key| names in any row version at all. */
static bool
handle_delta_any(const RootKey *key, uint64 handle, PLHash *digest)
{
	Relation	catalog = table_open(DoltHandleRelationId, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(catalog);
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;

	ScanKeyInit(&keys[0], Anum_pg_dolt_handle_hdlspace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->space));
	ScanKeyInit(&keys[1], Anum_pg_dolt_handle_hdlfile, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->file));
	ScanKeyInit(&keys[2], Anum_pg_dolt_handle_hdlbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(key->branch));
	ScanKeyInit(&keys[3], Anum_pg_dolt_handle_hdlhandle, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64) handle));
	scan = systable_beginscan(catalog, DoltHandleKeyIndexId, true, SnapshotAny, 4, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;
		bytea	   *bytes = DatumGetByteaPP(heap_getattr(tuple, Anum_pg_dolt_handle_hdldigest, desc, &isnull));

		if (VARSIZE_ANY_EXHDR(bytes) == PL_HASH_SIZE)
		{
			memcpy(digest->bytes, VARDATA_ANY(bytes), PL_HASH_SIZE);
			found = true;
		}
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	return found;
}

typedef struct HandleChange
{
	PLHash		digest;
	int64		delta;
} HandleChange;

typedef struct HandleChanges
{
	HandleChange *items;
	int			count;
	int			capacity;
} HandleChanges;

static PLStatus
collect_handle_change(void *arg, PLSlice key, PLValue before, PLValue after)
{
	HandleChanges *changes = arg;
	int64		delta = (after.present ? (int64) pl_read64(after.bytes.data) : 0) -
		(before.present ? (int64) pl_read64(before.bytes.data) : 0);

	if (!delta)
		return PL_OK;
	if (changes->count == changes->capacity)
	{
		changes->capacity = changes->capacity ? changes->capacity * 2 : 64;
		changes->items = changes->items ? repalloc_array(changes->items, HandleChange, changes->capacity) :
			palloc_array(HandleChange, changes->capacity);
	}
	memcpy(changes->items[changes->count].digest.bytes, key.data, PL_HASH_SIZE);
	changes->items[changes->count++].delta = delta;
	return PL_OK;
}

/* A fresh allocated handle for |s|: from the storage's counter, or the state's own. */
static uint64
handle_allocate(Relation rel, DGState *s)
{
	if (s->delta_private)
	{
		state_delta(s);
		return s->nextid++;
	}
	return dg_allocate_handle(rel);
}

/* The row bytes of |digest| in |tree|, copied; false if it holds no such row. */
static bool
row_of_digest(const PLTree *tree, PLHash digest, PLSlice *row)
{
	PLValue		value;
	uint8	   *copy;

	dg_check(pl_get(tree, (PLSlice) {digest.bytes, PL_HASH_SIZE}, &value));
	if (!value.present || value.bytes.len < 8)
		return false;
	copy = palloc(Max(value.bytes.len - 8, 1));
	memcpy(copy, value.bytes.data + 8, value.bytes.len - 8);
	row->data = copy;
	row->len = value.bytes.len - 8;
	return true;
}

/*
 * The row version |handle| names, live or not, as a fetch under SnapshotAny
 * wants it: the executor fetches versions it saw under an older snapshot and
 * versions this transaction removed. The latest state is tried first, then
 * the active snapshot's, then the delta's record of a removed allocated handle with its row from any state
 * holding it.
 */
bool
dg_handle_version(Relation rel, uint64 handle, PLHash *digest, PLSlice *row)
{
	RootKey		key = root_key(rel, dg_relation_branch(rel));
	DGBatch    *b = batch_find(&key);
	DGState    *s;
	bool		found;

	/*
	 * A version the pending batch added or removed is answered from the
	 * batch: loading the table for it would flush the batch, and an AFTER
	 * trigger fetching each row a statement wrote would then apply the
	 * batch once per row.
	 */
	if (b && b->table)
	{
		PendingHandle *mapping = hash_search(b->handles, &handle, HASH_FIND, NULL);
		PendingRow *pending = mapping ? hash_search(b->rows, &mapping->digest, HASH_FIND, NULL) : NULL;

		if (pending)
		{
			uint8	   *copy = palloc(Max(pending->row.len, 1));

			memcpy(copy, pending->row.data, pending->row.len);
			*digest = mapping->digest;
			row->data = copy;
			row->len = pending->row.len;
			return true;
		}
	}
	s = dg_load_tid(rel, handle);
	found = dg_handle_digest(s, handle, digest) && row_of_digest(&s->content, *digest, row);

	dg_release(s);
	if (found)
		return true;
	if (ActiveSnapshotSet())
	{
		s = dg_load(rel, GetActiveSnapshot());
		found = dg_handle_digest(s, handle, digest) && row_of_digest(&s->content, *digest, row);
		dg_release(s);
		if (found)
			return true;
	}

	/* Writes built on the snapshot conflict with versions committed since, which this still finds. */
	if (dg_writes_on_snapshot(rel))
	{
		s = dg_load_committed_latest(rel);
		found = dg_handle_digest(s, handle, digest) && row_of_digest(&s->content, *digest, row);
		dg_release(s);
		if (found)
			return true;
	}
	if (handle_is_derived(handle) || !handle_delta_any(&key, handle, digest))
		return false;
	s = dg_load(rel, SnapshotSelf);
	found = row_of_digest(&s->content, *digest, row);
	dg_release(s);
	if (!found && ActiveSnapshotSet())
	{
		s = dg_load(rel, GetActiveSnapshot());
		found = row_of_digest(&s->content, *digest, row);
		dg_release(s);
	}
	return found;
}

typedef struct InstallExtras
{
	Relation	rel;
	DGState    *s;
	const RootKey *key;
	CommandId	cid;
} InstallExtras;

static PLStatus
collect_install_extras(void *arg, PLSlice key, PLSlice value)
{
	InstallExtras *extras = arg;
	int64		count = value.len >= 8 ? (int64) pl_read64(value.data) : 0;
	PLHash		digest;
	uint64	   *allocated;
	int			n;

	if (count <= 1)
		return PL_OK;
	memcpy(digest.bytes, key.data, PL_HASH_SIZE);
	n = delta_handles(extras->s, digest, &allocated);
	while (count - n > 1)
	{
		handle_delta_edit(extras->s, extras->key, dg_allocate_handle(extras->rel), digest, true, extras->cid);
		n = delta_handles(extras->s, digest, &allocated);
	}
	return PL_OK;
}

/*
 * A storage with no roots of its own, whose content came from history, has no
 * handle rows for its duplicate rows yet: allocate them before its first save.
 * Distinct rows, the vast majority, need nothing.
 */
static void
handle_install_extras(Relation rel, DGState *s, const RootKey *key, CommandId cid)
{
	InstallExtras extras = {rel, s, key, cid};

	dg_check(pl_scan(&s->content, NULL, NULL, collect_install_extras, &extras));
}

/*
 * Replace |s|'s content with |contents|, keeping the handles of occurrences
 * present in both: only rows the diff adds or removes gain or lose handles,
 * and only allocated ones are written. Cost is proportional to the
 * difference, not the table.
 */
void
dg_handle_install(Relation rel, DGState *s, PLTree contents)
{
	PLTree		before = s->content;
	HandleChanges changes = {0};
	RootKey		key = root_key(rel, dg_relation_branch(rel));
	CommandId	cid = GetCurrentCommandId(true);

	if (!s->has_root && !s->delta_private)
		handle_install_extras(rel, s, &key, cid);
	dg_check(pl_diff(&before, &contents, collect_handle_change, &changes));
	for (int i = 0; i < changes.count; i++)
	{
		HandleChange *change = &changes.items[i];
		uint64	   *allocated;
		int			n = delta_handles(s, change->digest, &allocated);

		CHECK_FOR_INTERRUPTS();
		if (change->delta < 0)
		{
			/* Allocated occurrences go first; the derived one needs no write. */
			for (int64 j = 0; j < -change->delta && n > 0; j++)
			{
				uint64		handle = allocated[n - 1];

				handle_delta_edit(s, &key, handle, change->digest, false, cid);
				n = delta_handles(s, change->digest, &allocated);
			}
		}
		else
		{
			PLValue		value;
			int64		count;

			dg_check(pl_get(&before, (PLSlice) {change->digest.bytes, PL_HASH_SIZE}, &value));
			count = value.present ? (int64) pl_read64(value.bytes.data) : 0;
			for (int64 j = 0; j < change->delta; j++)
			{
				uint64		handle = count - n >= 1 ? handle_allocate(rel, s) : dg_handle_choose(rel, s, NULL, change->digest);

				handle_delta_edit(s, &key, handle, change->digest, true, cid);
				count++;
				n = delta_handles(s, change->digest, &allocated);
			}
		}
	}
	s->content = contents;
	if (changes.items)
		pfree(changes.items);
}
