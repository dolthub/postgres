/* ---------------------------------------------------------------------------
 * The row journal: small changes appended, folded into the trees on read
 * ---------------------------------------------------------------------------
 *
 * A command's batch of a few rows would rewrite the path to each row's leaf
 * in every tree the table has, and every rewritten node is a chunk written
 * to the heap and the WAL. Instead a small batch appends one journal row per
 * row occurrence it adds or removes, to pg_dolt_journal, keyed like the
 * table's roots by (storage, branch). The trees in the roots row stay as
 * they were.
 *
 * A reader folds the journal into the trees in memory: loading a table's or
 * an index's state applies the journal entries its snapshot sees to the
 * trees the roots name, and the resulting nodes live in the working store,
 * pinned, under a per-backend cache keyed by the roots and by generation
 * counters, so a statement that finds nothing changed pays nothing and one
 * that finds new entries applies only those. Every consumer of trees, from
 * scans to commits, therefore sees the folded trees without knowing about
 * the journal.
 *
 * The fold is persisted when a state built on folded entries is saved: a
 * large batch, a history install or a batch that would push the stream past
 * its bound saves roots that include the entries, and deletes those entries
 * in the same transaction. The entries a state folded are recorded in it for
 * that purpose, so concurrent folders of one key delete exactly what they
 * folded, in sequence order, and skip rows another folder already deleted.
 *
 * Nothing canonical is derived from a handle: the content and index trees
 * are folded from row bytes and digests only; handles reach the handle and
 * key trees alone.
 */

/* Rows in a batch above which the batch is applied to the trees directly. */
#define DG_JOURNAL_BATCH_MAX 64

/* Entries a stream may hold; a batch that would exceed it folds instead. */
#define DG_JOURNAL_STREAM_MAX 256

/* A storage's shared word: a generation, and committers under way in the low bits. */
#define JOURNAL_WORD_GENERATION	(UINT64CONST(1) << 16)
#define JOURNAL_WORD_COMMITTERS	(JOURNAL_WORD_GENERATION - 1)

typedef struct JournalEntry
{
	int64		seq;
	uint64		handle;
	bool		added;
	PLHash		digest;
	PLSlice		row;			/* when added; else empty */
} JournalEntry;

typedef struct JournalEntries
{
	JournalEntry *items;
	ItemPointerData *tids;		/* of the catalog rows, when read from the catalog */
	int			count,
				capacity;
} JournalEntries;

/*
 * A fold's cache key: the state's key and the kind of load. Loads under an
 * MVCC snapshot and of the latest state see different sets of a
 * transaction's own entries, and a writer alternates between them every
 * statement, so each kind keeps its own fold.
 */
typedef struct JournalFoldKey
{
	RootKey		key;
	int			latest;			/* 1: the latest state; 0: an MVCC snapshot */
	int			pad;
} JournalFoldKey;

/* The folded trees of one state key, kept across statements while valid. */
typedef struct JournalFold
{
	JournalFoldKey key;			/* hash key */
	RootKey		table_key;		/* whose journal applies */
	uint64		store_epoch;
	uint64		roots_generation;
	uint64		local_version;
	Visibility	seen;
	bool		table;
	PLHash		base;			/* the tree the entries were applied to */
	PLHash		folded;			/* the result */
	int64	   *seqs;			/* entries applied, ascending */
	int			seq_count,
				seq_capacity;
	PLHash	   *pinned;			/* nodes kept for the result, one pin each */
	int			pinned_count,
				pinned_capacity;
	HTAB	   *pinned_set;		/* the same, to pin each node once */
} JournalFold;

static HTAB *journal_folds;
/*
 * What this backend wrote to the journal, as fold validity sees it: an epoch
 * that moves when a transaction ends, which may discard or reveal writes to
 * any storage, and a version per storage that moves with each write to that
 * storage's stream. A write to one table leaves the folds of the others valid.
 */
static uint64 journal_local_epoch = 1;

typedef struct JournalVersion
{
	Oid			space;			/* hash key */
	RelFileNumber file;
	uint64		version;
	CommandId	last_cid;		/* the command of this transaction's last write to the stream */
} JournalVersion;

static HTAB *journal_versions;

static JournalVersion *
journal_version_entry(Oid space, RelFileNumber file, bool enter)
{
	struct
	{
		Oid			space;
		RelFileNumber file;
	}			key = {space, file};

	if (!journal_versions)
	{
		HASHCTL		ctl = {0};

		if (!enter)
			return NULL;
		ctl.keysize = sizeof(key);
		ctl.entrysize = sizeof(JournalVersion);
		ctl.hcxt = TopMemoryContext;
		journal_versions = hash_create("prolly journal versions", 32, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	return hash_search(journal_versions, &key, enter ? HASH_ENTER : HASH_FIND, NULL);
}

/* The version a fold of |table_key|'s stream must have been built at to stand. */
static uint64
journal_local_version_of(const RootKey *table_key)
{
	JournalVersion *entry = journal_version_entry(table_key->space, table_key->file, false);

	return (journal_local_epoch << 32) + (entry ? entry->version : 0);
}

/* This backend changed |space|/|file|'s stream in command |cid|. */
static void
journal_local_write(Oid space, RelFileNumber file, CommandId cid)
{
	JournalVersion *entry = journal_version_entry(space, file, true);

	if (entry->space != space || entry->file != file || entry->version == 0)
	{
		entry->space = space;
		entry->file = file;
		entry->version = 0;
	}
	entry->version++;
	entry->last_cid = cid;
}

/*
 * Whether a fold built under |built| sees the same stream as a load under
 * |now|: the same snapshot but for a later command, when this transaction
 * wrote nothing to the stream since the fold was built (its version stands)
 * and all it wrote before was visible to the fold. A later command sees
 * more of the transaction's own writes and nothing else.
 */
static bool
journal_same_stream(const Visibility *built, const Visibility *now, const RootKey *table_key)
{
	Visibility	a = *built,
				b = *now;
	JournalVersion *entry;

	if (built->overflow || now->overflow || built->cid == InvalidCommandId || now->cid == InvalidCommandId ||
		now->cid < built->cid)
		return false;
	a.cid = b.cid = 0;
	if (memcmp(&a, &b, sizeof(a)) != 0)
		return false;
	entry = journal_version_entry(table_key->space, table_key->file, false);
	return !entry || entry->version == 0 || entry->last_cid < built->cid;
}

/* Storages this transaction journaled, whose readers must notice at commit. */
typedef struct JournalTouched
{
	Oid			db,
				space;
	RelFileNumber file;
	bool		counted;		/* counted as a committer in the storage's word */
	uint64		before;			/* the word before */
} JournalTouched;

static JournalTouched *journal_touched;
static int	journal_touched_count,
			journal_touched_capacity;
static bool journal_committing;	/* past pre-commit: a new touch counts at once */
static List *journal_retired;	/* row memory of dropped stream caches, freed at transaction end */
static void journal_count_committer(JournalTouched *touched);
static void journal_streams_drop(Oid space, RelFileNumber file);
static pg_atomic_uint64 *journal_word(Oid space, RelFileNumber file);

/* A subtransaction rolled back: its writes leave every stream it touched. */
static void
journal_local_invalidate(void)
{
	journal_local_epoch++;
	for (int i = 0; i < journal_touched_count; i++)
		journal_streams_drop(journal_touched[i].space, journal_touched[i].file);
}

static HandleSlot *handle_slot_claim(Oid db, Oid space, RelFileNumber file);
static PLTree journal_table_base(Relation table, const RootKey *table_key, Snapshot snapshot, const char *branch, DGState **state);
static RootKey journal_index_table(Relation index, const char *branch, Relation *table);
static void journal_state_seqs(DGState *s, const JournalFold *fold);

static void
journal_touch(Oid db, Oid space, RelFileNumber file, CommandId cid)
{
	MemoryContext old = MemoryContextSwitchTo(TopTransactionContext);

	for (int i = 0; i < journal_touched_count; i++)
		if (journal_touched[i].db == db && journal_touched[i].space == space && journal_touched[i].file == file)
		{
			MemoryContextSwitchTo(old);
			journal_local_write(space, file, cid);
			return;
		}
	if (journal_touched_count == journal_touched_capacity)
	{
		journal_touched_capacity = journal_touched_capacity ? journal_touched_capacity * 2 : 8;
		journal_touched = journal_touched ? repalloc(journal_touched, journal_touched_capacity * sizeof(JournalTouched)) :
			palloc(journal_touched_capacity * sizeof(JournalTouched));
	}
	journal_touched[journal_touched_count].db = db;
	journal_touched[journal_touched_count].space = space;
	journal_touched[journal_touched_count].file = file;
	journal_touched[journal_touched_count].counted = false;
	journal_touched_count++;
	journal_local_write(space, file, cid);
	MemoryContextSwitchTo(old);

	/* Claim the storage's slot now: the commit must find its word. */
	(void) journal_word(space, file);
	if (journal_committing)
		journal_count_committer(&journal_touched[journal_touched_count - 1]);
}

/* |touched|'s storage has a committer under way: this transaction. */
static void
journal_count_committer(JournalTouched *touched)
{
	touched->before = pg_atomic_fetch_add_u64(journal_word(touched->space, touched->file), JOURNAL_WORD_GENERATION + 1);
	touched->counted = true;
}

/*
 * Before the commit becomes visible, count it as a committer of every storage
 * whose streams it changed: a reader then neither builds a cache nor keeps
 * one until the commit is visible and counted out.
 */
static void
journal_pre_commit(void)
{
	for (int i = 0; i < journal_touched_count; i++)
		if (!journal_touched[i].counted)
			journal_count_committer(&journal_touched[i]);
	journal_committing = true;
}

static void journal_stream_committed(Oid space, RelFileNumber file, uint64 before, uint64 after);

/*
 * At transaction end: count this transaction out of the storages it counted
 * itself a committer of, which moves their generation again. Its caches of
 * their streams stay current after a commit no other committer interleaved
 * with, its own entries now committed; any other end drops them.
 */
static void
journal_xact_end(bool committed)
{
	ListCell   *lc;

	/* The slots were claimed at the touch: finding them reads no catalogs. */
	shared_handles();
	for (int i = 0; i < journal_touched_count; i++)
	{
		JournalTouched *touched = &journal_touched[i];
		HandleSlot *slot = handle_slot_find(touched->db, touched->space, touched->file);
		uint64		after = 0;

		if (slot && touched->counted)
			after = pg_atomic_fetch_add_u64(&slot->journal_generation, JOURNAL_WORD_GENERATION - 1);
		else if (slot && committed)
			pg_atomic_fetch_add_u64(&slot->journal_generation, JOURNAL_WORD_GENERATION);
		if (committed && touched->counted)
			journal_stream_committed(touched->space, touched->file, touched->before, after);
		else
			journal_streams_drop(touched->space, touched->file);
	}
	journal_touched = NULL;
	journal_touched_count = journal_touched_capacity = 0;
	journal_committing = false;
	journal_local_epoch++;
	foreach(lc, journal_retired)
		MemoryContextDelete(lfirst(lc));
	list_free(journal_retired);
	journal_retired = NIL;
}

static void
journal_scan_keys(const RootKey *key, ScanKeyData *keys)
{
	ScanKeyInit(&keys[0], Anum_pg_dolt_journal_jrnspace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->space));
	ScanKeyInit(&keys[1], Anum_pg_dolt_journal_jrnfile, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(key->file));
	ScanKeyInit(&keys[2], Anum_pg_dolt_journal_jrnbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(key->branch));
}

/* The entries of |key|'s stream that |snapshot| sees, in sequence order. */
static JournalEntries
journal_read(const RootKey *key, Snapshot snapshot)
{
	Relation	catalog = table_open(DoltJournalRelationId, AccessShareLock);
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	JournalEntries entries = {0};
	TupleDesc	desc = RelationGetDescr(catalog);
	bool		registered = IsMVCCSnapshot(snapshot);
	bool		pushed = false;

	/* A scan under SnapshotSelf still needs some snapshot to bound its horizon. */
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (registered)
		snapshot = RegisterSnapshot(snapshot);
	journal_scan_keys(key, keys);
	scan = systable_beginscan(catalog, DoltJournalKeyIndexId, true, snapshot, 3, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_dolt_journal form = (Form_pg_dolt_journal) GETSTRUCT(tuple);
		JournalEntry *entry;
		bool		isnull;
		Datum		value;
		bytea	   *bytes;

		if (entries.count == entries.capacity)
		{
			entries.capacity = entries.capacity ? entries.capacity * 2 : 32;
			entries.items = entries.items ? repalloc(entries.items, entries.capacity * sizeof(JournalEntry)) :
				palloc(entries.capacity * sizeof(JournalEntry));
			entries.tids = entries.tids ? repalloc(entries.tids, entries.capacity * sizeof(ItemPointerData)) :
				palloc(entries.capacity * sizeof(ItemPointerData));
		}
		entries.tids[entries.count] = tuple->t_self;
		entry = &entries.items[entries.count++];
		entry->seq = form->jrnseq;
		entry->handle = (uint64) form->jrnhandle;
		entry->added = form->jrnkind == 'i';
		value = heap_getattr(tuple, Anum_pg_dolt_journal_jrndigest, desc, &isnull);
		bytes = DatumGetByteaPP(value);
		if (isnull || VARSIZE_ANY_EXHDR(bytes) != PL_HASH_SIZE)
			elog(ERROR, "invalid prolly journal digest");
		memcpy(entry->digest.bytes, VARDATA_ANY(bytes), PL_HASH_SIZE);
		entry->row = (PLSlice) {NULL, 0};
		value = heap_getattr(tuple, Anum_pg_dolt_journal_jrnrow, desc, &isnull);
		if (!isnull)
		{
			bytes = DatumGetByteaPP(value);
			entry->row.len = VARSIZE_ANY_EXHDR(bytes);
			entry->row.data = palloc(Max(entry->row.len, 1));
			memcpy(unconstify(uint8 *, entry->row.data), VARDATA_ANY(bytes), entry->row.len);
		}
		else if (entry->added)
			elog(ERROR, "prolly journal addition without its row");
	}
	systable_endscan(scan);
	if (registered)
		UnregisterSnapshot(snapshot);
	if (pushed)
		PopActiveSnapshot();
	table_close(catalog, AccessShareLock);
	return entries;
}

/* ---------------------------------------------------------------------------
 * The stream cache: a backend's decoded copy of each stream it reads
 * ---------------------------------------------------------------------------
 *
 * A load folds the entries of its table's stream that its snapshot sees, and
 * a writer's every new snapshot would scan the stream's rows from the catalog
 * again though almost all of them are the ones the last load read. Instead a
 * backend keeps the entries it read, decoded, with what it later wrote and
 * deleted itself, and answers loads from them while no other transaction has
 * changed the stream.
 *
 * Other transactions' changes show in a word per storage in shared memory: a
 * generation in the high bits and, in the low bits, the number of
 * transactions committing changes to the storage's streams. A committer adds
 * one to both before its commit becomes visible and moves the second back
 * after: while the word stands still and shows no committer, no transaction
 * has made a change to the storage's streams visible.
 *
 * A cache is built only from a word showing no committer, and holds the
 * entries committed when it was read: those that a snapshot taken right after
 * the word was read sees. A later snapshot that sees all that one did sees the
 * same, and the backend's own changes, by command. Prepared transactions
 * become visible in another backend, which cannot account for them, and a
 * standby replays commits without any: neither uses the cache.
 */

typedef struct StreamEntry
{
	JournalEntry entry;
	ItemPointerData tid;
	bool		own;			/* written by this transaction */
	CommandId	added;			/* when own: the command that wrote it */
	CommandId	removed;		/* the command of this transaction that deleted it, or invalid */
} StreamEntry;

typedef struct JournalStream
{
	RootKey		key;			/* hash key */
	MemoryContext memory;		/* rows */
	StreamEntry *items;
	int			count,
				capacity;
	uint64		word;			/* the storage's word the entries are current for */
	TransactionId xmax;			/* the snapshot the entries were read under */
	TransactionId *xip;
	int			xcnt;
	bool		valid;
} JournalStream;

static HTAB *journal_streams;

static bool
journal_cache_enabled(void)
{
	return max_prepared_xacts == 0 && !RecoveryInProgress() && !IsParallelWorker();
}

static pg_atomic_uint64 *
journal_word(Oid space, RelFileNumber file)
{
	return &handle_slot_claim(MyDatabaseId, space, file)->journal_generation;
}

static JournalStream *
journal_stream_find(const RootKey *key, bool enter)
{
	bool		found;
	JournalStream *stream;

	if (!journal_streams)
	{
		HASHCTL		ctl = {0};

		if (!enter)
			return NULL;
		ctl.keysize = sizeof(RootKey);
		ctl.entrysize = sizeof(JournalStream);
		ctl.hcxt = TopMemoryContext;
		journal_streams = hash_create("prolly journal streams", 32, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	stream = hash_search(journal_streams, key, enter ? HASH_ENTER : HASH_FIND, &found);
	if (stream && !found)
	{
		memset(((char *) stream) + sizeof(RootKey), 0, sizeof(*stream) - sizeof(RootKey));
		stream->memory = AllocSetContextCreate(TopMemoryContext, "prolly journal stream", ALLOCSET_SMALL_SIZES);
	}
	return stream;
}

/* Forget |stream|'s entries. Rows a load still holds stay until transaction end. */
static void
journal_stream_drop(JournalStream *stream)
{
	MemoryContext old;

	if (!stream->valid)
		return;
	old = MemoryContextSwitchTo(TopMemoryContext);
	journal_retired = lappend(journal_retired, stream->memory);
	MemoryContextSwitchTo(old);
	stream->memory = AllocSetContextCreate(TopMemoryContext, "prolly journal stream", ALLOCSET_SMALL_SIZES);
	stream->items = NULL;
	stream->count = stream->capacity = 0;
	stream->xip = NULL;
	stream->valid = false;
}

/* Drop the caches of every stream of storage |space|/|file|, or of all with InvalidOid. */
static void
journal_streams_drop(Oid space, RelFileNumber file)
{
	HASH_SEQ_STATUS status;
	JournalStream *stream;

	if (!journal_streams)
		return;
	hash_seq_init(&status, journal_streams);
	while ((stream = hash_seq_search(&status)) != NULL)
		if (!OidIsValid(space) || (stream->key.space == space && stream->key.file == file))
			journal_stream_drop(stream);
}

/* Whether |snapshot| sees every transaction that |stream|'s read saw as finished. */
static bool
journal_stream_covers(const JournalStream *stream, Snapshot snapshot)
{
	if (snapshot->snapshot_type != SNAPSHOT_MVCC || snapshot->suboverflowed ||
		TransactionIdPrecedes(snapshot->xmax, stream->xmax))
		return false;
	for (uint32 i = 0; i < snapshot->xcnt + snapshot->subxcnt; i++)
	{
		TransactionId xid = i < snapshot->xcnt ? snapshot->xip[i] : snapshot->subxip[i - snapshot->xcnt];
		bool		running = false;

		if (!TransactionIdPrecedes(xid, stream->xmax))
			continue;
		for (int j = 0; j < stream->xcnt && !running; j++)
			running = TransactionIdEquals(stream->xip[j], xid);
		if (!running)
			return false;
	}
	return true;
}

/* Insert |item| after the entries of |stream| with sequences up to its own. */
static void
journal_stream_insert(JournalStream *stream, const StreamEntry *item)
{
	int			at = stream->count;

	if (stream->count == stream->capacity)
	{
		MemoryContext old = MemoryContextSwitchTo(stream->memory);

		stream->capacity = stream->capacity ? stream->capacity * 2 : 64;
		stream->items = stream->items ? repalloc(stream->items, stream->capacity * sizeof(StreamEntry)) :
			palloc(stream->capacity * sizeof(StreamEntry));
		MemoryContextSwitchTo(old);
	}
	while (at > 0 && stream->items[at - 1].entry.seq > item->entry.seq)
		at--;
	memmove(&stream->items[at + 1], &stream->items[at], (stream->count - at) * sizeof(StreamEntry));
	stream->items[at] = *item;
	stream->count++;
}

/*
 * Read |key|'s stream into its cache, as of a snapshot taken after the word
 * was read: false when a committer is under way, or this transaction has
 * already changed the storage's streams, whose rows the cache would have to
 * take with their commands.
 */
static bool
journal_stream_build(JournalStream *stream, const RootKey *key)
{
	uint64		word = pg_atomic_read_u64(journal_word(key->space, key->file));
	Snapshot	latest;
	JournalEntries entries;
	MemoryContext old;

	if (word & JOURNAL_WORD_COMMITTERS)
		return false;
	for (int i = 0; i < journal_touched_count; i++)
		if (journal_touched[i].space == key->space && journal_touched[i].file == key->file)
			return false;
	pg_memory_barrier();
	latest = RegisterSnapshot(GetLatestSnapshot());
	if (latest->suboverflowed)
	{
		UnregisterSnapshot(latest);
		return false;
	}
	old = MemoryContextSwitchTo(stream->memory);
	entries = journal_read(key, latest);
	stream->items = palloc(Max(entries.count, 16) * sizeof(StreamEntry));
	stream->capacity = Max(entries.count, 16);
	stream->count = 0;
	for (int i = 0; i < entries.count; i++)
	{
		StreamEntry *item = &stream->items[stream->count++];

		item->entry = entries.items[i];
		item->tid = entries.tids[i];
		item->own = false;
		item->added = item->removed = InvalidCommandId;
	}
	stream->xmax = latest->xmax;
	stream->xcnt = latest->xcnt + latest->subxcnt;
	stream->xip = palloc(Max(stream->xcnt, 1) * sizeof(TransactionId));
	memcpy(stream->xip, latest->xip, latest->xcnt * sizeof(TransactionId));
	memcpy(stream->xip + latest->xcnt, latest->subxip, latest->subxcnt * sizeof(TransactionId));
	MemoryContextSwitchTo(old);
	UnregisterSnapshot(latest);
	stream->word = word;
	stream->valid = true;
	return true;
}

/*
 * The entries of |key|'s stream that |snapshot| sees, in sequence order:
 * from the cache when it is current and the snapshot sees all its read did,
 * else from the catalog. The rows stay valid until transaction end.
 */
static JournalEntries
journal_entries(const RootKey *key, Snapshot snapshot)
{
	JournalStream *stream;
	JournalEntries result = {0};
	bool		self = snapshot == SnapshotSelf;
	CommandId	cid;

	if (!journal_cache_enabled() || (!self && snapshot->snapshot_type != SNAPSHOT_MVCC))
		return journal_read(key, snapshot);
	stream = journal_stream_find(key, true);
	if (stream->valid && pg_atomic_read_u64(journal_word(key->space, key->file)) != stream->word)
		journal_stream_drop(stream);
	if (!stream->valid && !journal_stream_build(stream, key))
		return journal_read(key, snapshot);
	if (!self && !journal_stream_covers(stream, snapshot))
		return journal_read(key, snapshot);

	/*
	 * SnapshotSelf sees every committed entry, which while the word stands are
	 * those the cache read, and all of this transaction's changes.
	 */
	cid = self ? InvalidCommandId : snapshot->curcid;
	result.items = palloc(Max(stream->count, 1) * sizeof(JournalEntry));
	result.capacity = Max(stream->count, 1);
	for (int i = 0; i < stream->count; i++)
	{
		StreamEntry *item = &stream->items[i];

		if (item->removed != InvalidCommandId && (self || item->removed < cid))
			continue;
		if (item->own && !self && item->added >= cid)
			continue;
		result.items[result.count++] = item->entry;
	}
	return result;
}

/* This transaction appended an entry to |key|'s stream in command |cid|. */
static void
journal_stream_appended(const RootKey *key, const JournalEntry *entry, ItemPointer tid, CommandId cid)
{
	JournalStream *stream = journal_stream_find(key, false);
	StreamEntry item = {0};
	MemoryContext old;

	if (!stream || !stream->valid)
		return;
	item.entry = *entry;
	if (entry->row.len)
	{
		old = MemoryContextSwitchTo(stream->memory);
		item.entry.row.data = palloc(entry->row.len);
		memcpy(unconstify(uint8 *, item.entry.row.data), entry->row.data, entry->row.len);
		MemoryContextSwitchTo(old);
	}
	item.tid = *tid;
	item.own = true;
	item.added = cid;
	item.removed = InvalidCommandId;
	journal_stream_insert(stream, &item);
}

/* This transaction deleted the entry at |tid| of |key|'s stream in command |cid|. */
static void
journal_stream_removed(const RootKey *key, ItemPointer tid, CommandId cid)
{
	JournalStream *stream = journal_stream_find(key, false);

	if (!stream || !stream->valid)
		return;
	for (int i = 0; i < stream->count; i++)
		if (ItemPointerEquals(&stream->items[i].tid, tid) && stream->items[i].removed == InvalidCommandId)
		{
			stream->items[i].removed = cid;
			return;
		}
	journal_stream_drop(stream);
}

/* A commit left |space|/|file|'s word |after| + 1 from |before|: whether only this transaction moved it. */
static void
journal_stream_committed(Oid space, RelFileNumber file, uint64 before, uint64 after)
{
	HASH_SEQ_STATUS status;
	JournalStream *stream;

	if (!journal_streams)
		return;
	hash_seq_init(&status, journal_streams);
	while ((stream = hash_seq_search(&status)) != NULL)
	{
		int			kept = 0;

		if (stream->key.space != space || stream->key.file != file || !stream->valid)
			continue;
		if (stream->word != before || after != before + JOURNAL_WORD_GENERATION + 1)
		{
			journal_stream_drop(stream);
			continue;
		}
		for (int i = 0; i < stream->count; i++)
		{
			if (stream->items[i].removed != InvalidCommandId)
				continue;
			stream->items[kept] = stream->items[i];
			stream->items[kept].own = false;
			stream->items[kept].added = InvalidCommandId;
			kept++;
		}
		stream->count = kept;
		stream->word = before + 2 * JOURNAL_WORD_GENERATION;
	}
}

/*
 * Append one entry as of command |cid|, the command whose batch it records:
 * a later command's load may flush that batch, and must then see its rows.
 * A removal records the digest only.
 */
static void
journal_append_entry(Relation catalog, Relation index, IndexInfo *info, Relation oid_index, IndexInfo *oid_info, CommandId cid, const RootKey *key,
					 uint64 handle, bool added, PLHash digest, PLSlice row)
{
	Datum		index_values[4];
	bool		index_nulls[4] = {false};
	Datum		values[Natts_pg_dolt_journal];
	bool		nulls[Natts_pg_dolt_journal] = {false};
	bytea	   *digest_bytes = palloc(VARHDRSZ + PL_HASH_SIZE);
	HeapTuple	tuple;

	SET_VARSIZE(digest_bytes, VARHDRSZ + PL_HASH_SIZE);
	memcpy(VARDATA(digest_bytes), digest.bytes, PL_HASH_SIZE);
	values[Anum_pg_dolt_journal_oid - 1] = ObjectIdGetDatum(GetNewOidWithIndex(catalog, DoltJournalOidIndexId, Anum_pg_dolt_journal_oid));
	values[Anum_pg_dolt_journal_jrnspace - 1] = ObjectIdGetDatum(key->space);
	values[Anum_pg_dolt_journal_jrnfile - 1] = ObjectIdGetDatum(key->file);
	values[Anum_pg_dolt_journal_jrnseq - 1] = Int64GetDatum((int64) (handle * 2 + (added ? 0 : 1)));
	values[Anum_pg_dolt_journal_jrnhandle - 1] = Int64GetDatum((int64) handle);
	values[Anum_pg_dolt_journal_jrnkind - 1] = CharGetDatum(added ? 'i' : 'd');
	values[Anum_pg_dolt_journal_jrnbranch - 1] = CStringGetTextDatum(key->branch);
	values[Anum_pg_dolt_journal_jrndigest - 1] = PointerGetDatum(digest_bytes);
	if (added)
	{
		bytea	   *row_bytes = palloc(VARHDRSZ + row.len);

		SET_VARSIZE(row_bytes, VARHDRSZ + row.len);
		memcpy(VARDATA(row_bytes), row.data, row.len);
		values[Anum_pg_dolt_journal_jrnrow - 1] = PointerGetDatum(row_bytes);
	}
	else
		nulls[Anum_pg_dolt_journal_jrnrow - 1] = true;
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	heap_insert(catalog, tuple, cid, 0, NULL);
	{
		JournalEntry entry = {
			.seq = (int64) (handle * 2 + (added ? 0 : 1)),
			.handle = handle,
			.added = added,
			.digest = digest,
			.row = added ? row : (PLSlice) {NULL, 0},
		};

		journal_stream_appended(key, &entry, &tuple->t_self, cid);
	}
	index_values[0] = values[Anum_pg_dolt_journal_jrnspace - 1];
	index_values[1] = values[Anum_pg_dolt_journal_jrnfile - 1];
	index_values[2] = values[Anum_pg_dolt_journal_jrnbranch - 1];
	index_values[3] = values[Anum_pg_dolt_journal_jrnseq - 1];
	index_insert(index, index_values, index_nulls, &tuple->t_self, catalog, UNIQUE_CHECK_NO, false, info);
	index_values[0] = values[Anum_pg_dolt_journal_oid - 1];
	index_insert(oid_index, index_values, index_nulls, &tuple->t_self, catalog, UNIQUE_CHECK_YES, false, oid_info);
	heap_freetuple(tuple);
}

/*
 * Journal a table batch instead of applying it: one entry per handle the
 * batch adds or removes. Returns false, leaving the batch untouched, when the
 * stream would grow past its bound and the batch should fold instead.
 */
static bool
journal_batch(DGBatch *b, bool forced)
{
	HASH_SEQ_STATUS status;
	PendingHandle *mapping;
	Relation	catalog,
				index,
				oid_index;
	IndexInfo  *info,
			   *oid_info;
	long		n = hash_get_num_entries(b->handles);
	bool		pushed = false;

	/*
	 * A batch flushed inside a later subtransaction belongs to an earlier one:
	 * rows written now would die with this subtransaction, so it folds, under
	 * its own subtransaction, as roots do.
	 */
	if (b->subxid != GetCurrentSubTransactionId())
		return false;

	/*
	 * A command that already journaled this table journals the rest too:
	 * rows it wrote cannot be deleted by the same command, so it cannot fold.
	 */
	if (!forced && (n > DG_JOURNAL_BATCH_MAX || n + b->state->journal_count > DG_JOURNAL_STREAM_MAX))
		return false;
	if (XactReadOnly)
		ereport(ERROR, (errcode(ERRCODE_READ_ONLY_SQL_TRANSACTION), errmsg("cannot journal prolly rows in a read-only transaction")));

	/* The catalog's TOAST table needs a snapshot; a pre-commit flush may have none. */
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	catalog = table_open(DoltJournalRelationId, RowExclusiveLock);
	index = index_open(DoltJournalKeyIndexId, RowExclusiveLock);
	oid_index = index_open(DoltJournalOidIndexId, RowExclusiveLock);
	oid_info = BuildIndexInfo(oid_index);
	info = BuildIndexInfo(index);
	hash_seq_init(&status, b->handles);
	while ((mapping = hash_seq_search(&status)) != NULL)
	{
		PLSlice		row = {NULL, 0};

		if (mapping->present)
		{
			PendingRow *pending = hash_search(b->rows, &mapping->digest, HASH_FIND, NULL);

			if (!pending)
				elog(ERROR, "prolly batch handle without its row");
			row = pending->row;
		}
		journal_append_entry(catalog, index, info, oid_index, oid_info, b->cid, &b->key, mapping->handle, mapping->present, mapping->digest, row);
	}
	index_close(oid_index, RowExclusiveLock);
	index_close(index, RowExclusiveLock);
	table_close(catalog, RowExclusiveLock);
	if (pushed)
		PopActiveSnapshot();
	handle_delta_write(&b->key, b->handles, b->cid);
	journal_touch(MyDatabaseId, b->key.space, b->key.file, b->cid);
	return true;
}

/*
 * Delete the entries a state folded, in sequence order. A row another
 * transaction deleted meanwhile was folded by it too, and is skipped.
 */
static void
journal_consume(const RootKey *key, const int64 *seqs, int count, CommandId cid)
{
	Relation	catalog;
	ScanKeyData keys[3];
	SysScanDesc scan;
	HeapTuple	tuple;
	int			i = 0;
	bool		pushed = false;

	if (!count)
		return;
	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	catalog = table_open(DoltJournalRelationId, RowExclusiveLock);
	journal_scan_keys(key, keys);
	scan = systable_beginscan(catalog, DoltJournalKeyIndexId, true, SnapshotSelf, 3, keys);
	while (i < count && HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		int64		seq = ((Form_pg_dolt_journal) GETSTRUCT(tuple))->jrnseq;
		TM_FailureData tmfd;
		TM_Result	result;

		while (i < count && seqs[i] < seq)
			i++;
		if (i == count || seqs[i] != seq)
			continue;
		i++;
		result = heap_delete(catalog, &tuple->t_self, cid, InvalidSnapshot, true, &tmfd, false);
		if (result != TM_Ok && result != TM_Deleted && result != TM_Updated && result != TM_SelfModified)
			elog(ERROR, "could not delete a prolly journal entry");
		if (result == TM_Ok)
			journal_stream_removed(key, &tuple->t_self, cid);
		else
			journal_streams_drop(key->space, key->file);
	}
	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);
	if (pushed)
		PopActiveSnapshot();
	journal_touch(MyDatabaseId, key->space, key->file, cid);
}

/* ---------------------------------------------------------------------------
 * Folding entries into trees
 * ---------------------------------------------------------------------------
 */

static int
compare_journal_digest(const void *a, const void *b)
{
	return memcmp((*(const JournalEntry *const *) a)->digest.bytes, (*(const JournalEntry *const *) b)->digest.bytes, PL_HASH_SIZE);
}

/* Apply |entries| to a table's content tree: one edit per digest. */
static void
journal_apply_table(PLStore *store, PLTree *tree, const JournalEntries *entries)
{
	JournalEntry **order = palloc(Max(entries->count, 1) * sizeof(JournalEntry *));
	PLEdit	   *edits = palloc(Max(entries->count, 1) * sizeof(PLEdit));
	int			n = 0;

	for (int i = 0; i < entries->count; i++)
		order[i] = &entries->items[i];
	qsort(order, entries->count, sizeof(JournalEntry *), compare_journal_digest);

	/* Content: one edit per digest, from the stored count plus the net change. */
	for (int i = 0; i < entries->count;)
	{
		JournalEntry *first = order[i];
		int64		delta = 0;
		PLSlice		row = first->row;
		PLValue		old;
		int64		count;

		for (; i < entries->count && memcmp(order[i]->digest.bytes, first->digest.bytes, PL_HASH_SIZE) == 0; i++)
		{
			delta += order[i]->added ? 1 : -1;
			if (order[i]->added)
				row = order[i]->row;
		}
		count = tree_count(tree, (PLSlice) {first->digest.bytes, PL_HASH_SIZE}, &old) + delta;
		if (count < 0)
			elog(ERROR, "prolly journal removes a missing row");
		if (!row.data && old.present)
			row = (PLSlice) {old.bytes.data + 8, old.bytes.len - 8};
		if (count > 0 && !row.data)
			elog(ERROR, "prolly journal row is unavailable");
		if (delta == 0)
			continue;
		edits[n].key = (PLSlice) {first->digest.bytes, PL_HASH_SIZE};
		edits[n].value.present = count > 0;
		edits[n].value.bytes = count > 0 ? count_value(count, row) : (PLSlice) {NULL, 0};
		n++;
	}
	if (n)
		dg_check(pl_apply(tree, edits, n, tree));
	pfree(edits);
	pfree(order);
}

static const PLEdit *sort_edit_order;

static int
compare_edit_order(const void *a, const void *b)
{
	return pl_compare(sort_edit_order[*(const int *) a].key, sort_edit_order[*(const int *) b].key);
}

/*
 * Apply |entries| to an index's tree: each entry's index key, from its row.
 * A removal's row comes from an addition of the same row anywhere in the
 * stream (|all|), else from the table's content as it was before the stream.
 */
static void
journal_apply_index(Relation index, PLStore *store, PLTree *tree, const JournalEntries *entries,
					const JournalEntries *all, const PLTree *content)
{
	PLEdit	   *edits = palloc(Max(entries->count, 1) * sizeof(PLEdit));
	int64	   *deltas = palloc(Max(entries->count, 1) * sizeof(int64));
	PLSlice    *rows = palloc(Max(entries->count, 1) * sizeof(PLSlice));
	int			n = 0;

	for (int i = 0; i < entries->count; i++)
	{
		JournalEntry *entry = &entries->items[i];
		PLSlice		row = entry->row;
		StringInfoData key;

		if (!row.data)
		{
			/*
			 * A removal's row comes from an addition of the same row in the
			 * stream, else from the table's content tree.
			 */
			PLValue		value;

			for (int j = 0; j < all->count && !row.data; j++)
				if (all->items[j].added && memcmp(all->items[j].digest.bytes, entry->digest.bytes, PL_HASH_SIZE) == 0)
					row = all->items[j].row;
			if (!row.data)
			{
				dg_check(pl_get(content, (PLSlice) {entry->digest.bytes, PL_HASH_SIZE}, &value));
				if (!value.present || value.bytes.len < 8)
					elog(ERROR, "prolly journal removes a row the index never held");
				row = (PLSlice) {value.bytes.data + 8, value.bytes.len - 8};
			}
		}
		initStringInfo(&key);
		dg_index_entry_key(index, row, entry->digest, &key);
		edits[n].key = (PLSlice) {(uint8 *) key.data, key.len};
		deltas[n] = entry->added ? 1 : -1;
		rows[n] = row;
		n++;
	}
	if (n)
	{
		int		   *order = palloc(n * sizeof(int));
		PLEdit	   *merged = palloc(n * sizeof(PLEdit));
		int			m = 0;

		for (int i = 0; i < n; i++)
			order[i] = i;
		sort_edit_order = edits;
		qsort(order, n, sizeof(int), compare_edit_order);
		for (int i = 0; i < n;)
		{
			PLSlice		key = edits[order[i]].key;
			PLSlice		row = rows[order[i]];
			int64		delta = 0;
			PLValue		old;
			int64		count;

			for (; i < n && pl_compare(edits[order[i]].key, key) == 0; i++)
				delta += deltas[order[i]];
			if (delta == 0)
				continue;
			count = tree_count(tree, key, &old) + delta;
			if (count < 0)
				elog(ERROR, "prolly journal removes a missing index entry");
			merged[m].key = key;
			merged[m].value.present = count > 0;
			merged[m].value.bytes = count > 0 ? count_value(count, row) : (PLSlice) {NULL, 0};
			m++;
		}
		if (m)
			dg_check(pl_apply(tree, merged, m, tree));
		pfree(order);
		pfree(merged);
	}
	pfree(edits);
	pfree(deltas);
	pfree(rows);
}

/* ---------------------------------------------------------------------------
 * The fold cache
 * ---------------------------------------------------------------------------
 */

static JournalFold *
journal_fold_find(const RootKey *key, bool latest, bool enter)
{
	JournalFoldKey fold_key;

	memset(&fold_key, 0, sizeof(fold_key));
	fold_key.key = *key;
	fold_key.latest = latest ? 1 : 0;
	if (!journal_folds)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(JournalFoldKey);
		ctl.entrysize = sizeof(JournalFold);
		ctl.hcxt = CacheMemoryContext;
		journal_folds = hash_create("prolly journal folds", 32, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	return hash_search(journal_folds, &fold_key, enter ? HASH_ENTER : HASH_FIND, NULL);
}

/* Release the nodes a fold kept; they drop with the next transaction if unwritten. */
static void
journal_fold_unpin(JournalFold *fold)
{
	if (fold->store_epoch == store_epoch && working_store)
		for (int i = 0; i < fold->pinned_count; i++)
		{
			PLNode	   *node = pl_find_local(working_store, fold->pinned[i]);

			if (node)
				pl_node_unpin(working_store, node);
		}
	fold->pinned_count = 0;
	if (fold->pinned_set)
		hash_destroy(fold->pinned_set);
	fold->pinned_set = NULL;
}

static void
journal_fold_pin_tree(JournalFold *fold, PLStore *store, PLHash root)
{
	PLNode	   *node = pl_find_local(store, root);
	bool		found;

	/*
	 * Another fold may pin the same unwritten nodes, as folds of one table
	 * under different snapshots share subtrees: each fold pins a node once
	 * and releases its own pin, so neither drops the other's nodes.
	 */
	if (!node || node->backed)
		return;
	if (!fold->pinned_set)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(PLHash);
		ctl.entrysize = sizeof(PLHash);
		ctl.hcxt = CacheMemoryContext;
		fold->pinned_set = hash_create("prolly fold pins", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	hash_search(fold->pinned_set, &root, HASH_ENTER, &found);
	if (found)
		return;
	check_stack_depth();
	for (uint32 i = 0; node->level && i < node->count; i++)
	{
		PLHash		child;

		child = pl_child_hash(node, i);
		journal_fold_pin_tree(fold, store, child);
	}
	pl_node_pin(store, node);
	if (fold->pinned_count == fold->pinned_capacity)
	{
		MemoryContext old = MemoryContextSwitchTo(CacheMemoryContext);

		fold->pinned_capacity = fold->pinned_capacity ? fold->pinned_capacity * 2 : 64;
		fold->pinned = fold->pinned ? repalloc(fold->pinned, fold->pinned_capacity * sizeof(PLHash)) :
			palloc(fold->pinned_capacity * sizeof(PLHash));
		MemoryContextSwitchTo(old);
	}
	fold->pinned[fold->pinned_count++] = root;
}

/* Give |s| the folded tree |root|, and the sequences it folds. */
static void
journal_fold_install(DGState *s, PLHash root, const JournalFold *fold)
{
	s->content.root = root;
	journal_state_seqs(s, fold);
}

/* A state's own copy of the sequences its trees fold, outliving the cache entry. */
static void
journal_state_seqs(DGState *s, const JournalFold *fold)
{
	s->journal_count = fold->seq_count;
	s->journal_seqs = palloc(Max(fold->seq_count, 1) * sizeof(int64));
	memcpy(s->journal_seqs, fold->seqs, fold->seq_count * sizeof(int64));
}

/* Whether every applied sequence of |fold| is among |entries| (both ascending). */
static bool
journal_fold_covered(const JournalFold *fold, const JournalEntries *entries)
{
	int			j = 0;

	for (int i = 0; i < fold->seq_count; i++)
	{
		while (j < entries->count && entries->items[j].seq < fold->seqs[i])
			j++;
		if (j == entries->count || entries->items[j].seq != fold->seqs[i])
			return false;
		j++;
	}
	return true;
}

/*
 * Fold the journal of |table| into |s|, loaded for |rel| under |snapshot|:
 * |rel| is the table itself or one of its indexes. The state's trees are
 * replaced by the folded ones, and the entries folded are recorded in it, so
 * that saving the state deletes them.
 */
static void
journal_fold_state(Relation rel, const RootKey *table_key_in, Relation table, const char *branch, Snapshot snapshot, DGState *s)
{
	RootKey		key = root_key(rel, branch),
				table_key = *table_key_in;
	bool		is_table = rel->rd_rel->relkind != RELKIND_INDEX;
	uint64		roots_gen = roots_generation(&key) + roots_generation(&table_key);
	Visibility	seen;
	bool		latest = !(snapshot && IsMVCCSnapshot(snapshot));
	JournalFold *fold = journal_fold_find(&key, latest, false);
	PLHash		current = s->content.root;
	JournalEntries entries;
	MemoryContext old;
	JournalFold *sibling;

	if (s->store != dg_working_store())
		return;
	visibility_capture(snapshot, &seen);

	/* Nothing moved since the last fold: its result stands. */
	if (fold && fold->store_epoch == store_epoch && fold->roots_generation == roots_gen &&
		(visibility_equal(&fold->seen, &seen) || journal_same_stream(&fold->seen, &seen, &table_key)) &&
		fold->local_version == journal_local_version_of(&table_key) &&
		fold->table == is_table && pl_hash_equal(fold->base, current))
	{
		if (fold->seq_count)
			journal_fold_install(s, fold->folded, fold);
		return;
	}

	entries = journal_entries(&table_key, snapshot ? snapshot : SnapshotSelf);
	if (!fold)
	{
		fold = journal_fold_find(&key, latest, true);
		memset(((char *) fold) + sizeof(JournalFoldKey), 0, sizeof(*fold) - sizeof(JournalFoldKey));
	}

	/*
	 * An empty stream is the common case for a read-mostly table: record it
	 * under the same identity as a fold, so the next load with nothing moved
	 * skips the catalog scan too.
	 */
	if (!entries.count)
	{
		journal_fold_unpin(fold);
		fold->base = current;
		fold->folded = current;
		fold->seq_count = 0;
		fold->store_epoch = store_epoch;
		fold->roots_generation = roots_gen;
		fold->local_version = journal_local_version_of(&table_key);
		fold->seen = seen;
		fold->table = is_table;
		fold->table_key = table_key;
		s->journal_seqs = NULL;
		s->journal_count = 0;
		return;
	}

	/*
	 * The other kind of load's fold of the same trees may have folded exactly
	 * these entries: a writer's loads see the same stream once its commands
	 * catch up with it. Take its result rather than apply them again.
	 */
	sibling = journal_fold_find(&key, !latest, false);
	if (sibling && sibling->seq_count == entries.count && sibling->store_epoch == store_epoch &&
		sibling->roots_generation == roots_gen && pl_hash_equal(sibling->base, current) &&
		sibling->table == is_table && journal_fold_covered(sibling, &entries))
	{
		if (!(fold->seq_count && fold->store_epoch == store_epoch && fold->roots_generation == roots_gen &&
			  pl_hash_equal(fold->base, current) && fold->table == is_table))
		{
			journal_fold_unpin(fold);
			fold->base = current;
		}
		current = sibling->folded;
	}

	/* Continue from the last result when it is a prefix of what is visible now. */
	else if (fold->seq_count && fold->store_epoch == store_epoch && fold->roots_generation == roots_gen &&
		pl_hash_equal(fold->base, current) && fold->table == is_table &&
		journal_fold_covered(fold, &entries))
	{
		JournalEntries rest = {0};

		rest.items = palloc(Max(entries.count, 1) * sizeof(JournalEntry));
		for (int i = 0, j = 0; i < entries.count; i++)
		{
			while (j < fold->seq_count && fold->seqs[j] < entries.items[i].seq)
				j++;
			if (j < fold->seq_count && fold->seqs[j] == entries.items[i].seq)
			{
				j++;			/* a sequence may repeat: a row removed and added back */
				continue;
			}
			rest.items[rest.count++] = entries.items[i];
		}
		current = fold->folded;
		if (rest.count)
		{
			PLTree		tree = {s->store, current};

			if (is_table)
				journal_apply_table(s->store, &tree, &rest);
			else
			{
				DGState    *table_state;
				PLTree		content = journal_table_base(table, &table_key, snapshot, branch, &table_state);

				journal_apply_index(rel, s->store, &tree, &rest, &entries, &content);
				if (table_state)
					dg_release(table_state);
			}
			current = tree.root;
		}
	}
	else
	{
		PLTree		tree = {s->store, current};

		journal_fold_unpin(fold);
		fold->base = s->content.root;
		if (is_table)
			journal_apply_table(s->store, &tree, &entries);
		else
		{
			DGState    *table_state;
			PLTree		content = journal_table_base(table, &table_key, snapshot, branch, &table_state);

			journal_apply_index(rel, s->store, &tree, &entries, &entries, &content);
			if (table_state)
				dg_release(table_state);
		}
		current = tree.root;
		fold->seq_count = 0;
	}

	/* Record the result and keep its nodes. */
	old = MemoryContextSwitchTo(CacheMemoryContext);
	if (fold->seq_capacity < entries.count)
	{
		fold->seq_capacity = Max(entries.count, 64);
		fold->seqs = fold->seqs ? repalloc(fold->seqs, fold->seq_capacity * sizeof(int64)) : palloc(fold->seq_capacity * sizeof(int64));
	}
	MemoryContextSwitchTo(old);
	for (int i = 0; i < entries.count; i++)
		fold->seqs[i] = entries.items[i].seq;
	fold->seq_count = entries.count;
	fold->folded = current;
	fold->store_epoch = store_epoch;
	fold->roots_generation = roots_gen;
	fold->local_version = journal_local_version_of(&table_key);
	fold->seen = seen;
	fold->table = is_table;
	fold->table_key = table_key;
	journal_fold_pin_tree(fold, s->store, current);
	journal_fold_install(s, current, fold);
}

/*
 * The table's trees before its stream, for an index fold's removals. The
 * load must not flush the table's current batch, which the command is still
 * filling: that is the batch loader's rule, borrowed here.
 */
static PLTree
journal_table_base(Relation table, const RootKey *table_key, Snapshot snapshot, const char *branch, DGState **state)
{
	bool		was_loading = loading_for_batch;
	bool		foldable;

	*state = NULL;
	if (!table)
	{
		/* A bound index knows its table only by locator: read its roots directly. */
		Roots		base,
					view = view_roots(table_key, snapshot, &base);

		return view.present ? tree_at(view.content) : empty_tree();
	}
	loading_for_batch = true;
	PG_TRY();
	{
		*state = load_unfolded(table, snapshot, &foldable);
	}
	PG_FINALLY();
	{
		loading_for_batch = was_loading;
	}
	PG_END_TRY();
	return (*state)->content;
}

/*
 * The journal key of the table |index| orders. An ordinary index names a
 * table relation, opened for the caller to close; a bound index names a
 * canonical handle, whose storage the binding catalog locates.
 */
static RootKey
journal_index_table(Relation index, const char *branch, Relation *table)
{
	RelFileLocator locator;

	if (!dg_storage_bound(index))
	{
		*table = relation_open(index->rd_index->indrelid, NoLock);
		return root_key(*table, branch);
	}
	*table = NULL;
	dg_storage_table_locator(index, &locator);
	return root_key_of(locator.spcOid, locator.relNumber, branch);
}

/* The state of |rel| folded with its table's journal, for the public loaders. */
static DGState *
journal_fold_loaded(Relation rel, const char *branch, Snapshot snapshot, DGState *s)
{
	Relation	table = NULL;
	RootKey		table_key;

	if (!s || (!DG_ROWS_KIND(rel->rd_rel->relkind) && rel->rd_rel->relkind != RELKIND_INDEX))
		return s;
	if (rel->rd_rel->relkind == RELKIND_INDEX)
		table_key = journal_index_table(rel, branch, &table);
	else
		table_key = root_key(rel, branch);
	if (batch_group_snapshot && loading_for_batch)
		snapshot = batch_group_snapshot;
	journal_fold_state(rel, &table_key, table, branch, snapshot, s);
	if (table)
		relation_close(table, NoLock);
	return s;
}

/* The roots every fold keeps alive, for the node garbage collector. */
static void
journal_fold_roots(PLHash **roots, int *count, int *capacity)
{
	HASH_SEQ_STATUS status;
	JournalFold *fold;

	if (!journal_folds)
		return;
	hash_seq_init(&status, journal_folds);
	while ((fold = hash_seq_search(&status)) != NULL)
		if (fold->store_epoch == store_epoch && fold->seq_count)
			add_roots(roots, count, capacity, &fold->folded, 1);
}

/* Whether a committed transaction other than this one journaled the addition of |handle|. */
static bool
journal_handle_committed(const RootKey *key, uint64 handle)
{
	Relation	catalog = table_open(DoltJournalRelationId, AccessShareLock);
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		committed = false;
	bool		pushed = false;

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	journal_scan_keys(key, keys);
	ScanKeyInit(&keys[3], Anum_pg_dolt_journal_jrnseq, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64) (handle * 2)));
	scan = systable_beginscan(catalog, DoltJournalKeyIndexId, true, SnapshotAny, 4, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		TransactionId xmin = HeapTupleHeaderGetXmin(tuple->t_data);

		if (!TransactionIdIsCurrentTransactionId(xmin) && TransactionIdDidCommit(xmin))
			committed = true;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	if (pushed)
		PopActiveSnapshot();
	return committed;
}

/* The addition entry naming |handle| in |key|'s stream, whatever its visibility: its digest and a copy of its row. */
static bool
journal_entry_any(const RootKey *key, uint64 handle, PLHash *digest, PLSlice *row)
{
	Relation	catalog = table_open(DoltJournalRelationId, AccessShareLock);
	TupleDesc	desc = RelationGetDescr(catalog);
	ScanKeyData keys[4];
	SysScanDesc scan;
	HeapTuple	tuple;
	bool		found = false;
	bool		pushed = false;

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	journal_scan_keys(key, keys);
	ScanKeyInit(&keys[3], Anum_pg_dolt_journal_jrnseq, BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum((int64) (handle * 2)));
	scan = systable_beginscan(catalog, DoltJournalKeyIndexId, true, SnapshotAny, 4, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		bool		isnull;
		Datum		value = heap_getattr(tuple, Anum_pg_dolt_journal_jrndigest, desc, &isnull);
		bytea	   *bytes = DatumGetByteaPP(value);
		bytea	   *rowbytes;
		uint8	   *copy;

		if (isnull || VARSIZE_ANY_EXHDR(bytes) != PL_HASH_SIZE)
			continue;
		value = heap_getattr(tuple, Anum_pg_dolt_journal_jrnrow, desc, &isnull);
		if (isnull)
			continue;
		rowbytes = DatumGetByteaPP(value);
		memcpy(digest->bytes, VARDATA_ANY(bytes), PL_HASH_SIZE);
		copy = palloc(Max(VARSIZE_ANY_EXHDR(rowbytes), 1));
		memcpy(copy, VARDATA_ANY(rowbytes), VARSIZE_ANY_EXHDR(rowbytes));
		row->data = copy;
		row->len = VARSIZE_ANY_EXHDR(rowbytes);
		found = true;
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	if (pushed)
		PopActiveSnapshot();
	return found;
}

/*
 * Whether any stream on |branch| has an entry |snapshot| sees. The catalog
 * holds only unfolded entries, a few hundred per journaled table at most,
 * so a scan is cheap.
 */
static bool
journal_branch_pending(const char *branch, Snapshot snapshot)
{
	Relation	catalog = table_open(DoltJournalRelationId, AccessShareLock);
	ScanKeyData key;
	SysScanDesc scan;
	bool		pending;
	bool		registered = snapshot && IsMVCCSnapshot(snapshot);
	bool		pushed = false;

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	if (registered)
		snapshot = RegisterSnapshot(snapshot);
	ScanKeyInit(&key, Anum_pg_dolt_journal_jrnbranch, BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(branch));
	scan = systable_beginscan(catalog, InvalidOid, false, registered ? snapshot : SnapshotSelf, 1, &key);
	pending = HeapTupleIsValid(systable_getnext(scan));
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	if (registered)
		UnregisterSnapshot(snapshot);
	if (pushed)
		PopActiveSnapshot();
	return pending;
}

/* The largest handle in any entry of |space|/|file| on any branch. */
static uint64
journal_max_handle(Oid space, RelFileNumber file)
{
	Relation	catalog = table_open(DoltJournalRelationId, AccessShareLock);
	ScanKeyData keys[2];
	SysScanDesc scan;
	HeapTuple	tuple;
	uint64		max = 0;

	bool		pushed = false;

	if (!ActiveSnapshotSet())
	{
		PushActiveSnapshot(GetTransactionSnapshot());
		pushed = true;
	}
	ScanKeyInit(&keys[0], Anum_pg_dolt_journal_jrnspace, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(space));
	ScanKeyInit(&keys[1], Anum_pg_dolt_journal_jrnfile, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(file));
	scan = systable_beginscan(catalog, DoltJournalKeyIndexId, true, SnapshotAny, 2, keys);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		TransactionId xmin = HeapTupleHeaderGetXmin(tuple->t_data);

		if (!TransactionIdIsCurrentTransactionId(xmin) && !TransactionIdIsInProgress(xmin) &&
			!TransactionIdDidCommit(xmin))
			continue;
		max = Max(max, (uint64) ((Form_pg_dolt_journal) GETSTRUCT(tuple))->jrnhandle);
	}
	systable_endscan(scan);
	table_close(catalog, AccessShareLock);
	if (pushed)
		PopActiveSnapshot();
	return max;
}
