/*-------------------------------------------------------------------------
 *
 * dg_view.c
 *	  Reading a prolly state: its base tree merged with the delta store.
 *
 * A loaded state is a base tree, which a root row names, and a view of the
 * delta store (dg_delta.c): the versions its snapshot sees and the base does
 * not hold. Readers merge the two as Doltgrust's readers merge a tree's base
 * with its memtable: a lookup reads the base and the delta's versions of the
 * key, a cursor walks the base and the delta's keys in step, reading the
 * delta's a chunk at a time. Nothing is hashed or rebuilt, and nothing is
 * read until asked for, so loading a state costs the same however large the
 * delta.
 *
 * Every tree's value is a count of occurrences followed by the row its key
 * names, so a change resolves against the base by adding counts; an entry
 * whose count reaches zero is gone.
 *
 * Callers that need a real tree, as version control and DDL do, take
 * dg_content, which applies every change to the base with one pl_apply and
 * remembers the root the state was loaded with, which a save diffs against.
 *
 * src/backend/access/dolt_prolly/dg_view.c
 *
 *-------------------------------------------------------------------------
 */
#include "dg.h"

#include "access/transam.h"
#include "common/hashfn.h"

#define DG_CURSOR_BASE		0
#define DG_CURSOR_CHANGE	1
#define DG_CURSOR_BOTH		2

/*
 * Changes a cursor reads ahead at first, doubling on each read up to the
 * most: a lookup of a few keys reads no more than it needs.
 */
#define DG_CURSOR_FIRST_CHUNK 4
#define DG_CURSOR_CHUNK 256

/* Changes gathered from the delta store into backend memory. */
typedef struct Gathered
{
	DGChange   *items;
	int			count,
				capacity;
	MemoryContext memory;
} Gathered;

static void
gather(void *arg, const DoltDeltaEdit *edit)
{
	Gathered   *g = arg;
	DGChange   *change;
	uint8	   *bytes;

	if (g->count == g->capacity)
	{
		g->capacity = g->capacity ? g->capacity * 2 : 16;
		g->items = g->items ? repalloc(g->items, g->capacity * sizeof(DGChange)) :
			MemoryContextAlloc(g->memory, g->capacity * sizeof(DGChange));
	}
	change = &g->items[g->count++];
	memset(change, 0, sizeof(*change));
	bytes = MemoryContextAlloc(g->memory, Max(edit->keylen + edit->rowlen, 1));
	memcpy(bytes, edit->key, edit->keylen);
	if (edit->rowlen)
		memcpy(bytes + edit->keylen, edit->row, edit->rowlen);
	change->key = (PLSlice) {bytes, edit->keylen};
	change->row = (PLSlice) {bytes + edit->keylen, edit->rowlen};
	change->delta = edit->delta;
}

/* The merged value of |change|, in |memory|: the base's count plus its delta, then the row. */
static DGChange *
resolve(DGState *s, DGChange *change, MemoryContext memory)
{
	PLValue		base;
	int64		count;
	PLSlice		row;
	uint8	   *bytes;

	if (change->resolved)
		return change;
	dg_check(pl_get(&s->tree, change->key, &base));
	count = (base.present ? (int64) pl_read64(base.bytes.data) : 0) + change->delta;
	if (base.present && change->row.len &&
		(base.bytes.len != change->row.len + 8 || memcmp(base.bytes.data + 8, change->row.data, change->row.len) != 0))
		elog(ERROR, "prolly delta names a different row than its tree");
	if (count < 0)
		elog(ERROR, "prolly delta removes more occurrences than its tree holds");
	change->resolved = true;
	change->present = count > 0;
	if (change->present)
	{
		row = base.present ? (PLSlice) {base.bytes.data + 8, base.bytes.len - 8} : change->row;
		bytes = MemoryContextAlloc(memory, 8 + row.len);
		pl_write64(bytes, (uint64) count);
		memcpy(bytes + 8, row.data, row.len);
		change->value = (PLSlice) {bytes, 8 + row.len};
	}
	return change;
}

bool
dg_get(DGState *s, PLSlice key, PLValue *out)
{
	if (s->changes)
	{
		DGChanges  *c = s->changes;
		Gathered	g;

		/*
		 * A lookup's value lives until four more lookups that find changes
		 * have been made, long enough for every caller to copy what it keeps.
		 */
		c->lookup = (c->lookup + 1) % lengthof(c->lookups);
		if (c->lookups[c->lookup])
			MemoryContextReset(c->lookups[c->lookup]);
		else
			c->lookups[c->lookup] = AllocSetContextCreate(c->memory, "prolly lookup", ALLOCSET_SMALL_SIZES);
		memset(&g, 0, sizeof(g));
		g.memory = c->lookups[c->lookup];
		if (dolt_delta_get(&c->view, key.data, key.len, gather, &g))
		{
			DGChange   *change = resolve(s, &g.items[0], g.memory);

			out->present = change->present;
			out->bytes = change->present ? change->value : (PLSlice) {NULL, 0};
			return out->present;
		}
	}
	dg_check(pl_get(&s->tree, key, out));
	return out->present;
}

/*
 * Read the cursor's next chunk of changes: from |bound| on the first read,
 * then past the last change it read.
 */
static void
cursor_read(DGCursor *c, const PLSlice *bound, bool inclusive)
{
	Gathered	g = {.memory = c->memory};
	PLSlice		last;

	if (c->count)
	{
		last = c->chunk[c->count - 1].key;
		bound = &last;
		inclusive = false;
	}
	if (!c->chunk_size)
		c->chunk_size = DG_CURSOR_FIRST_CHUNK;
	dolt_delta_range(&c->state->changes->view, bound ? bound->data : NULL, bound ? bound->len : 0,
					 inclusive, c->has_stop ? c->stop.data : NULL, c->has_stop ? c->stop.len : 0,
					 c->backward, c->chunk_size, gather, &g);
	c->chunk = g.items;
	c->count = g.count;
	c->next = 0;
	c->exhausted = g.count < c->chunk_size;
	c->chunk_size = Min(c->chunk_size * 2, DG_CURSOR_CHUNK);
}

/* The cursor's next change, reading ahead when its chunk runs out; NULL if none. */
static DGChange *
cursor_change(DGCursor *c)
{
	if (!c->state->changes)
		return NULL;
	if (c->next == c->count && !c->exhausted)
		cursor_read(c, NULL, false);
	return c->next < c->count ? &c->chunk[c->next] : NULL;
}

/* Settle |c| on the next entry in its direction that exists after merging. */
static void
cursor_settle(DGCursor *c)
{
	for (;;)
	{
		DGChange   *change = cursor_change(c);
		bool		has_base = c->base.valid;
		int			order;

		if (!has_base && !change)
		{
			c->valid = false;
			return;
		}
		if (!change)
			order = -1;
		else if (!has_base)
			order = 1;
		else
		{
			order = pl_compare(pl_cursor_entry(&c->base).key, change->key);
			if (c->backward)
				order = -order;
		}
		if (order < 0)
		{
			c->entry = pl_cursor_entry(&c->base);
			c->source = DG_CURSOR_BASE;
			c->valid = true;
			return;
		}
		resolve(c->state, change, c->memory);
		if (change->present)
		{
			c->entry.key = change->key;
			c->entry.value = change->value;
			c->source = order == 0 ? DG_CURSOR_BOTH : DG_CURSOR_CHANGE;
			c->valid = true;
			return;
		}

		/* The change removes the entry: step past it, and the base's too. */
		c->next++;
		if (order == 0)
			dg_check(c->backward ? pl_cursor_prev(&c->base) : pl_cursor_next(&c->base));
	}
}

/* Reset |c| for a new walk of |s|, keeping, and emptying, the memory it has. */
static void
cursor_reset(DGCursor *c, DGState *s)
{
	MemoryContext memory = c->memory;

	if (memory)
		MemoryContextReset(memory);
	else if (s->changes)
		memory = AllocSetContextCreate(s->changes->memory, "prolly cursor", ALLOCSET_SMALL_SIZES);
	memset(c, 0, sizeof(*c));
	c->memory = memory;
	c->state = s;
}

/* Release what |c| holds; it starts zeroed again. */
void
dg_cursor_end(DGCursor *c)
{
	if (c->memory)
		MemoryContextDelete(c->memory);
	memset(c, 0, sizeof(*c));
}

/* Remember where the walk ends: changes past it are never read. */
static void
cursor_stop(DGCursor *c, const PLSlice *stop)
{
	uint8	   *bytes;

	if (!stop || !c->memory)
		return;
	bytes = MemoryContextAlloc(c->memory, Max(stop->len, 1));
	memcpy(bytes, stop->data, stop->len);
	c->stop = (PLSlice) {bytes, stop->len};
	c->has_stop = true;
}

/* Walk |s| forward from |lower|; entries at or past |upper| may be left out. */
void
dg_cursor_init(DGCursor *c, DGState *s, const PLSlice *lower, const PLSlice *upper)
{
	cursor_reset(c, s);
	cursor_stop(c, upper);
	dg_check(pl_cursor_init(&c->base, &s->tree, lower));
	if (s->changes)
		cursor_read(c, lower, true);
	cursor_settle(c);
}

/* Walk |s| backward from below |upper|; entries below |lower| may be left out. */
void
dg_cursor_init_last(DGCursor *c, DGState *s, const PLSlice *upper, const PLSlice *lower)
{
	cursor_reset(c, s);
	c->backward = true;
	cursor_stop(c, lower);
	dg_check(pl_cursor_init_last(&c->base, &s->tree, upper));
	if (s->changes)
		cursor_read(c, upper, false);
	cursor_settle(c);
}

static void
cursor_step(DGCursor *c, bool backward)
{
	Assert(backward == c->backward);
	if (!c->valid)
		return;
	if (c->source != DG_CURSOR_CHANGE)
		dg_check(backward ? pl_cursor_prev(&c->base) : pl_cursor_next(&c->base));
	if (c->source != DG_CURSOR_BASE)
		c->next++;
	cursor_settle(c);
}

void
dg_cursor_next(DGCursor *c)
{
	cursor_step(c, false);
}

void
dg_cursor_prev(DGCursor *c)
{
	cursor_step(c, true);
}

PLEntry
dg_cursor_entry(const DGCursor *c)
{
	return c->entry;
}

/* Walk [lower, upper) with |c|, calling |callback| for each merged entry until it returns PL_STOP. */
static void
scan_with(DGCursor *c, DGState *s, const PLSlice *lower, const PLSlice *upper, PLScanFn callback, void *arg)
{
	for (dg_cursor_init(c, s, lower, upper); c->valid; dg_cursor_next(c))
	{
		PLStatus	status;

		if (upper && pl_compare(c->entry.key, *upper) >= 0)
			break;
		status = callback(arg, c->entry.key, c->entry.value);
		if (status == PL_STOP)
			break;
		dg_check(status);
	}
}

/*
 * Call |callback| for each merged entry with key in [lower, upper), until it
 * returns PL_STOP. The entries are gone when it returns: the callback copies
 * what it keeps.
 */
void
dg_scan(DGState *s, const PLSlice *lower, const PLSlice *upper, PLScanFn callback, void *arg)
{
	DGCursor	c = {0};

	scan_with(&c, s, lower, upper, callback, arg);
	dg_cursor_end(&c);
}

/* dg_scan whose entries last as long as |s|, for callers that keep them. */
void
dg_scan_keep(DGState *s, const PLSlice *lower, const PLSlice *upper, PLScanFn callback, void *arg)
{
	DGCursor	c = {0};

	scan_with(&c, s, lower, upper, callback, arg);
}

/* About how many entries the merged content holds: an estimate, for planning. */
uint64
dg_count(DGState *s)
{
	int64		count = (int64) pl_count(&s->tree);

	if (s->changes)
		count += dolt_delta_net(&s->changes->view.tree);
	return (uint64) Max(count, 0);
}

/*
 * An identity of the content a state shows: its base's root, marked with what
 * it sees of the delta. Equal identities show the same rows.
 */
PLHash
dg_state_view(const DGState *s)
{
	PLHash		view = s->tree.root;

	if (s->changes)
	{
		const DGChanges *c = s->changes;
		uint64		mark = hash_combine64(c->seq, ((uint64) c->snapshot.xmin << 32) | c->snapshot.xmax);

		mark = hash_combine64(mark, ((uint64) c->snapshot.curcid << 32) | ((uint64) c->snapshot.xcnt << 1) | c->view.own);
		for (uint32 i = 0; i < c->snapshot.xcnt; i++)
			mark = hash_combine64(mark, c->snapshot.xip[i]);
		for (int i = 0; i < 8; i++)
			view.bytes[i] ^= (uint8) (mark >> (i * 8));
	}
	return view;
}

/*
 * Give |s| a view of |tree|'s delta under |snapshot|, an MVCC snapshot,
 * leaving out what |base| holds and, unless |own|, this transaction's own
 * versions: none if the delta holds nothing.
 */
void
dg_state_view_delta(DGState *s, const DoltDeltaTree *tree, Snapshot snapshot, const DoltDeltaHorizon *base, bool own)
{
	DGChanges  *c;
	uint64		seq = dolt_delta_seq(tree);

	Assert(IsMVCCSnapshot(snapshot));
	if (seq <= 1)
		return;
	c = palloc0(sizeof(DGChanges));
	c->memory = CurrentMemoryContext;
	c->seq = seq;
	c->snapshot = *snapshot;
	c->snapshot.xip = palloc(Max(snapshot->xcnt, 1) * sizeof(TransactionId));
	memcpy(c->snapshot.xip, snapshot->xip, snapshot->xcnt * sizeof(TransactionId));
	c->snapshot.subxip = palloc(Max(snapshot->subxcnt, 1) * sizeof(TransactionId));
	memcpy(c->snapshot.subxip, snapshot->subxip, snapshot->subxcnt * sizeof(TransactionId));
	c->snapshot.regd_count = 0;
	c->snapshot.active_count = 0;
	c->snapshot.copied = true;
	c->view.tree = *tree;
	c->view.snapshot = &c->snapshot;
	c->view.own = own;
	if (base && TransactionIdIsValid(base->xmax))
	{
		c->horizon = *base;
		c->horizon.xip = palloc(Max(base->xcnt, 1) * sizeof(TransactionId));
		memcpy(c->horizon.xip, base->xip, base->xcnt * sizeof(TransactionId));
		c->view.base = &c->horizon;
	}
	s->changes = c;
	s->loaded_known = false;
}

/*
 * The whole content as one tree: the changes applied to the base. The first
 * call also fixes the root the state was loaded with, which a save diffs
 * against.
 */
PLTree *
dg_content(DGState *s)
{
	if (s->changes)
	{
		Gathered	g = {.memory = s->changes->memory};
		PLEdit	   *edits;

		dolt_delta_range(&s->changes->view, NULL, 0, true, NULL, 0, false, INT_MAX, gather, &g);
		edits = palloc(Max(g.count, 1) * sizeof(PLEdit));
		for (int i = 0; i < g.count; i++)
		{
			DGChange   *change = resolve(s, &g.items[i], g.memory);

			edits[i].key = change->key;
			edits[i].value.present = change->present;
			edits[i].value.bytes = change->present ? change->value : (PLSlice) {NULL, 0};
		}
		if (g.count)
			dg_check(pl_apply(&s->tree, edits, g.count, &s->tree));
		pfree(edits);
		s->changes = NULL;
	}
	if (!s->loaded_known)
	{
		s->loaded = s->tree.root;
		s->loaded_known = true;
	}
	return &s->tree;
}

/* Replace the whole content with |tree|, keeping the root the state was loaded with. */
void
dg_set_content(DGState *s, PLTree tree)
{
	(void) dg_content(s);
	s->tree = tree;
}
