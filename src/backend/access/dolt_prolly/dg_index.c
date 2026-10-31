#include "dg.h"
#include "access/doltbranch.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "access/amapi.h"
#include "access/nbtree.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/relscan.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "utils/datum.h"
#include "catalog/namespace.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_namespace_d.h"
#include "lib/stringinfo.h"
#include "nodes/pathnodes.h"
#include "nodes/tidbitmap.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/pg_locale.h"
#include "optimizer/optimizer.h"
#include "utils/selfuncs.h"
#include "fmgr.h"
#include "utils/fmgrprotos.h"
#include "common/hashfn.h"
#include "utils/rel.h"

typedef struct DGIndexEntry
{
	PLEntry		entry;
	size_t		prefix;
	bool		nullkey;
}			DGIndexEntry;
/*
 * A scan reads index entries one at a time, in the direction of its first
 * fetch, and keeps what it has produced, so that it can step back over it or
 * restore a mark. Each entry yields a TID per occurrence of its row.
 */
typedef struct DGIndexScan
{
	MemoryContext context;		/* holds the states and items, until rescan */
	Relation	bitmap_table;
	DGState    *index;
	DGState    *table;
	TupleTableSlot *slot;
	StringInfoData lower,
				upper;
	bool		has_lower,
				has_upper;
	PLCursor	cursor;
	bool		started;		/* cursor positioned */
	bool		backward;		/* direction items are produced in */
	bool		exhausted;
	ItemPointerData *tids;
	HeapTuple  *tuples;
	size_t		count,
				capacity;
	int64		position,		/* in production order */
				mark;
}			DGIndexScan;
static int
compare_entries(const void *a, const void *b)
{
	return pl_compare(((const DGIndexEntry *) a)->entry.key, ((const DGIndexEntry *) b)->entry.key);
}
static void
escaped(StringInfo out, PLSlice bytes, bool descending)
{
	size_t		i;
	uint8		xor = descending ? 255 : 0;

	for (i = 0; i < bytes.len; i++)
	{
		appendStringInfoCharMacro(out, bytes.data[i] ^ xor);
		if (!bytes.data[i])
			appendStringInfoCharMacro(out, 255 ^ xor);
	}
	appendStringInfoCharMacro(out, 0 ^ xor);
	appendStringInfoCharMacro(out, 0 ^ xor);
}
/*
 * The row digest an index key of |index| ends with, read back from its
 * escaped bytes: cheaper than hashing the row the value holds.
 */
static bool
key_digest(Relation index, PLSlice key, PLHash *digest)
{
	size_t		i = 0;
	int			j;

	for (j = 0; j < index->rd_index->indnkeyatts; j++)
	{
		bool		desc = (index->rd_indoption[j] & INDOPTION_DESC) != 0;
		uint8		xor = desc ? 255 : 0;

		if (i >= key.len)
			return false;
		if (key.data[i++] != 1)
			continue;			/* a null: its tag alone */
		for (;;)
		{
			if (i + 1 >= key.len)
				return false;
			if ((key.data[i] ^ xor) == 0)
			{
				i++;
				if ((key.data[i++] ^ xor) == 0)
					break;		/* the terminator */
				continue;		/* an escaped zero byte */
			}
			i++;
		}
	}
	for (j = 0; j < PL_HASH_SIZE; j++)
	{
		if (i >= key.len)
			return false;
		digest->bytes[j] = key.data[i++];
		if (!digest->bytes[j])
			i++;				/* the escape of a zero byte */
	}
	return i + 2 == key.len;
}

/*
 * The index entry key for table row |row| with digest |digest|: each key
 * column as a null tag and escaped canonical bytes, then the escaped digest.
 * *prefix receives the length of the key-column part, and *nullkey whether
 * any key column is null.
 */
/* Use the same expression evaluator as native index insertion. */
static void
index_values(Relation index, TupleTableSlot *slot, Datum *values, bool *nulls)
{
	if (RelationGetIndexExpressions(index) == NIL)
	{
		for (int j = 0; j < index->rd_index->indnatts; j++)
			values[j] = slot_getattr(slot, index->rd_index->indkey.values[j], &nulls[j]);
	}
	else
	{
		IndexInfo *info = BuildIndexInfo(index);
		EState *estate = CreateExecutorState();

		GetPerTupleExprContext(estate)->ecxt_scantuple = slot;
		FormIndexDatum(info, slot, estate, values, nulls);
		for (int j = 0; j < index->rd_index->indnatts; j++)
			if (!nulls[j])
			{
				Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(index), j);

				values[j] = datumCopy(values[j], attr->attbyval, attr->attlen);
			}
		FreeExecutorState(estate);
	}
}

bool
dg_index_row_matches(Relation table, Relation index, PLSlice row)
{
	List *predicate = RelationGetIndexPredicate(index);
	EState *estate;
	TupleTableSlot *slot;
	ItemPointerData tid;
	bool result;

	if (predicate == NIL)
		return true;
	estate = CreateExecutorState();
	slot = table_slot_create(table, NULL);
	ItemPointerSetInvalid(&tid);
	dg_decode(table, row, &tid, slot);
	GetPerTupleExprContext(estate)->ecxt_scantuple = slot;
	result = ExecQual(ExecPrepareQual(predicate, estate), GetPerTupleExprContext(estate));
	ExecDropSingleTupleTableSlot(slot);
	FreeExecutorState(estate);
	return result;
}

/* Keys and predicate membership are shared by writes, rebuilds and merges. */
bool
dg_index_row_key(Relation table, Relation index, PLSlice row, StringInfo key, bool *nullkey)
{
	TupleTableSlot *slot = NULL;
	Datum values[INDEX_MAX_KEYS];
	bool nulls[INDEX_MAX_KEYS];

	if (!dg_index_row_matches(table, index, row))
		return false;
	if (RelationGetIndexExpressions(index) != NIL)
	{
		ItemPointerData tid;

		slot = table_slot_create(table, NULL);
		ItemPointerSetInvalid(&tid);
		dg_decode(table, row, &tid, slot);
		index_values(index, slot, values, nulls);
	}
	*nullkey = false;
	for (int j = 0; j < index->rd_index->indnkeyatts; j++)
	{
		PLValue field;
		Oid type = TupleDescAttr(RelationGetDescr(index), j)->atttypid;
		bool desc = (index->rd_indoption[j] & INDOPTION_DESC) != 0;
		bool nullfirst = (index->rd_indoption[j] & INDOPTION_NULLS_FIRST) != 0;

		if (slot)
		{
			field.present = !nulls[j];
			if (field.present)
				field.bytes = dg_key_value(type, values[j]);
		}
		else
		{
			dg_check(pl_tuple_get(row, index->rd_index->indkey.values[j] - 1, &field));
			if (field.present)
				field.bytes = dg_key_field(type, field.bytes);
		}
		appendStringInfoChar(key, field.present ? 1 : (nullfirst ? 0 : 2));
		if (field.present)
			escaped(key, field.bytes, desc);
		else
			*nullkey = true;
	}
	if (slot)
		ExecDropSingleTupleTableSlot(slot);
	return true;
}

static bool
index_entry(Relation table, Relation index, PLSlice row, PLSlice digest, StringInfo key, size_t *prefix, bool *nullkey)
{
	if (!dg_index_row_key(table, index, row, key, nullkey))
		return false;
	*prefix = key->len;
	escaped(key, digest, false);
	return true;
}

/* The btree comparison function the key encoding of |type| orders as. */
static const char *
index_comparator(Oid type)
{
	if (OidIsValid(get_element_type(type)))
		return "btarraycmp";
	switch (get_typtype(type))
	{
		case TYPTYPE_COMPOSITE:
			return "btrecordcmp";
		case TYPTYPE_RANGE:
			return "range_cmp";
		case TYPTYPE_MULTIRANGE:
			return "multirange_cmp";
	}
	switch (type)
	{
		case OIDOID:
			return "btoidcmp";
		case BITOID:
			return "bitcmp";
		case VARBITOID:
			return "varbitcmp";
		case INETOID:
		case CIDROID:
			return "network_cmp";
		case JSONBOID:
			return "jsonb_cmp";
		case BOOLOID:
			return "btboolcmp";
		case INT2OID:
			return "btint2cmp";
		case INT4OID:
			return "btint4cmp";
		case INT8OID:
			return "btint8cmp";
		case TEXTOID:
		case VARCHAROID:
			return "bttextcmp";
		case BYTEAOID:
			return "byteacmp";
		case BPCHAROID:
			return "bpcharcmp";
		case CHAROID:
			return "btcharcmp";
		case NAMEOID:
			return "btnamecmp";
		case FLOAT4OID:
			return "btfloat4cmp";
		case FLOAT8OID:
			return "btfloat8cmp";
		case NUMERICOID:
			return "numeric_cmp";
		case DATEOID:
			return "date_cmp";
		case TIMEOID:
			return "time_cmp";
		case TIMETZOID:
			return "timetz_cmp";
		case TIMESTAMPOID:
			return "timestamp_cmp";
		case TIMESTAMPTZOID:
			return "timestamptz_cmp";
		case INTERVALOID:
			return "interval_cmp";
		case UUIDOID:
			return "uuid_cmp";
		case CASHOID:
			return "cash_cmp";
		case MACADDROID:
			return "macaddr_cmp";
		case MACADDR8OID:
			return "macaddr8_cmp";
		default:
			return "enum_cmp";
	}
}

/* Reject index definitions the encoding cannot order correctly. */
void
dg_index_validate(Relation table, Relation index)
{
	if (table->rd_tableam != dg_table_routine())
		dg_unsupported("prolly indexes on another table AM");
	if (index->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT)
		dg_unsupported("temporary or unlogged indexes");
	for (int j = 0; j < index->rd_index->indnkeyatts; j++)
	{
		int			att = index->rd_index->indkey.values[j];
		Oid			type,
					function = index->rd_support[j];
		const char *expected;
		static const char *operators[] = {"<", "<=", "=", ">=", ">"};
		static const char *pattern_operators[] = {"~<~", "~<=~", "=", "~>=~", "~>~"};
		bool pattern = false;
		int			strategy;

		if (att < 0)
			dg_unsupported("system-column indexes");
		type = getBaseType(TupleDescAttr(RelationGetDescr(index), j)->atttypid);
		if (!dg_key_ordered(type))
			dg_unsupported("ordered index keys of this type");
		if (OidIsValid(index->rd_indcollation[j]) &&
			!pg_newlocale_from_collation(index->rd_indcollation[j])->collate_is_c)
			dg_unsupported("non-C index collations");
		expected = index_comparator(type);
		if ((type == TEXTOID || type == VARCHAROID) && !strcmp(get_func_name(function), "bttext_pattern_cmp"))
		{
			expected = "bttext_pattern_cmp";
			pattern = true;
		}
		else if (type == BPCHAROID && !strcmp(get_func_name(function), "btbpchar_pattern_cmp"))
		{
			expected = "btbpchar_pattern_cmp";
			pattern = true;
		}
		if (get_func_namespace(function) != PG_CATALOG_NAMESPACE || strcmp(get_func_name(function), expected))
			dg_unsupported("custom index comparators");

		/* Operator classes are declared on varchar's text and an enum's anyenum. */
		if (OidIsValid(get_element_type(type)))
			type = ANYARRAYOID;
		else if (type == VARCHAROID)
			type = TEXTOID;
		else if (type == CIDROID)
			type = INETOID;
		else if (get_typtype(type) == TYPTYPE_ENUM)
			type = ANYENUMOID;
		else if (get_typtype(type) == TYPTYPE_COMPOSITE)
			type = RECORDOID;
		else if (get_typtype(type) == TYPTYPE_RANGE)
			type = ANYRANGEOID;
		else if (get_typtype(type) == TYPTYPE_MULTIRANGE)
			type = ANYMULTIRANGEOID;
		for (strategy = 1; strategy <= 5; strategy++)
		{
			Oid			actual = get_opfamily_member(index->rd_opfamily[j], type, type, strategy);
			Oid			wanted = OpernameGetOprid(list_make2(makeString("pg_catalog"), makeString(pstrdup((pattern ? pattern_operators : operators)[strategy - 1]))), type, type);

			if (!OidIsValid(actual) || actual != wanted)
				dg_unsupported("custom index operators");
		}
	}
	dg_validate_descriptor(RelationGetDescr(table));
}

void
dg_index_rebuild(Relation table, Relation index)
{
	DGState    *source,
			   *target;
	DGRows		rows;
	DGIndexEntry *sorted;
	PLEntry    *entries;
	size_t		i, used = 0;

	dg_index_validate(table, index);
	source = dg_load(table, SnapshotSelf);
	target = dg_load(index, SnapshotSelf);
	rows = dg_rows(&source->content);
	sorted = palloc0(Max(rows.count, 1) * sizeof(*sorted));
	entries = palloc0(Max(rows.count, 1) * sizeof(*entries));
	for (i = 0; i < rows.count; i++)
	{
		StringInfoData key;
		uint64		count = pl_read64(rows.items[i].value.data);
		PLSlice		row = {rows.items[i].value.data + 8, rows.items[i].value.len - 8};
		uint8	   *value;

		initStringInfo(&key);
		if (!index_entry(table, index, row, rows.items[i].key, &key, &sorted[used].prefix, &sorted[used].nullkey))
		{
			pfree(key.data);
			continue;
		}
		sorted[used].entry.key.data = (uint8 *) key.data;
		sorted[used].entry.key.len = key.len;
		value = palloc(8 + row.len);
		pl_write64(value, count);
		memcpy(value + 8, row.data, row.len);
		sorted[used].entry.value.data = value;
		sorted[used].entry.value.len = 8 + row.len;
		used++;
	}
	qsort(sorted, used, sizeof(*sorted), compare_entries);
	for (i = 0; i < used; i++)
	{
		if (index->rd_index->indisunique && (!sorted[i].nullkey || index->rd_index->indnullsnotdistinct))
		{
			bool		duplicate = pl_read64(sorted[i].entry.value.data) > 1;

			if (i && sorted[i].prefix == sorted[i - 1].prefix && memcmp(sorted[i].entry.key.data, sorted[i - 1].entry.key.data, sorted[i].prefix) == 0)
				duplicate = true;
			if (duplicate)
				ereport(ERROR, (errcode(ERRCODE_UNIQUE_VIOLATION), errmsg("duplicate key violates unique constraint \"%s\"", RelationGetRelationName(index))));
		}
		entries[i] = sorted[i].entry;
	}
	dg_check(pl_build(target->store, entries, used, &target->content));
	dg_save(index, target, GetCurrentCommandId(true));
	dg_release(source);
	dg_release(target);
}

/* The prolly indexes of |table| that writes maintain, opened. */
static List *
maintained_indexes(Relation table, bool *opened)
{
	List	   *result = NIL;
	List	   *indexes;
	ListCell   *cell;

	*opened = !dg_storage_bound(table);
	if (dg_storage_bound(table))
		return list_copy(dg_storage_indexes(table));
	indexes = RelationGetIndexList(table);
	foreach(cell, indexes)
	{
		Relation	index = index_open(lfirst_oid(cell), RowExclusiveLock);

		if (strcmp(get_am_name(index->rd_rel->relam), "dolt_prolly_index") != 0)
			dg_unsupported("other index families on prolly tables");
		if (index->rd_index->indisready)
			result = lappend(result, index);
		else
			index_close(index, RowExclusiveLock);
	}
	list_free(indexes);
	return result;
}

static void
close_indexes(List *indexes, bool opened)
{
	ListCell   *cell;

	if (opened)
		foreach(cell, indexes)
			index_close((Relation) lfirst(cell), RowExclusiveLock);
	list_free(indexes);
}

void
dg_rebuild_indexes(Relation rel)
{
	bool		opened;
	List	   *indexes = maintained_indexes(rel, &opened);
	ListCell   *cell;

	foreach(cell, indexes)
		dg_index_rebuild(rel, (Relation) lfirst(cell));
	close_indexes(indexes, opened);
}

static bool check_key(Relation table, Relation index, uint64 self, StringInfo key, PLSlice row, IndexUniqueCheck check);
static pg_noreturn void unique_violation(Relation table, Relation index, Datum *values, bool *isnull);

/*
 * Add |delta| occurrences of table row |row|, the version with |handle|, to
 * every maintained index of |table|. Each index entry carries an occurrence
 * count, like the content tree, so this is one path-local edit per index.
 * Keep the table command ID: a BEFORE trigger may have advanced the current
 * command, but these edits must be discarded when that table batch journals.
 */
void
dg_index_apply(Relation table, PLSlice row, int64 delta, uint64 handle, CommandId cid)
{
	bool		opened;
	List	   *indexes = maintained_indexes(table, &opened);
	PLHash		digest = dg_rowhash(row);
	PLSlice		digest_bytes = {digest.bytes, PL_HASH_SIZE};
	ListCell   *cell;

	foreach(cell, indexes)
	{
		Relation	index = lfirst(cell);
		DGBatch    *batch = dg_batch(index, cid);
		StringInfoData key;
		size_t		prefix;
		bool		nullkey;

		initStringInfo(&key);
		if (!index_entry(table, index, row, digest_bytes, &key, &prefix, &nullkey))
		{
			pfree(key.data);
			continue;
		}
		dg_batch_index(batch, (PLSlice) {(uint8 *) key.data, key.len}, prefix, digest, row, delta);
		/*
		 * Writers of bound canonical storage may bypass the executor's index
		 * insertion, so the AM checks their unique indexes itself.
		 */
		if (delta > 0 && dg_storage_bound(table) && index->rd_index->indisunique &&
			(!nullkey || index->rd_index->indnullsnotdistinct))
		{
			key.len = prefix;
			check_key(table, index, handle, &key, row, UNIQUE_CHECK_YES);
		}
		pfree(key.data);
	}
	close_indexes(indexes, opened);
}

bool
dg_index_entry_key(Relation table, Relation index, PLSlice row, PLHash digest, StringInfo key)
{
	size_t		prefix;
	bool		nullkey;

	return index_entry(table, index, row, (PLSlice) {digest.bytes, PL_HASH_SIZE}, key, &prefix, &nullkey);
}

/* The smallest key greater than every key that starts with |prefix|. */
static bool
prefix_successor(StringInfo prefix)
{
	while (prefix->len > 0 && (uint8) prefix->data[prefix->len - 1] == 0xFF)
		prefix->len--;
	if (prefix->len == 0)
		return false;
	prefix->data[prefix->len - 1]++;
	return true;
}

typedef struct HandleList
{
	uint64	   *items;
	int			count;
	int			capacity;
} HandleList;

/* The handles of every occurrence of row |digest| in |table|, |count| of them if known. */
static HandleList
handles_of(DGState *table, PLHash digest, int64 count)
{
	HandleList	list = {0};

	list.count = dg_handles_of(table, digest, count, &list.items, &list.capacity);
	return list;
}

typedef struct KeyRows
{
	Relation	index;
	PLHash	   *digests;
	int			count;
	int			capacity;
} KeyRows;

static PLStatus
collect_key_rows(void *arg, PLSlice key, PLSlice value)
{
	KeyRows    *rows = arg;

	if (rows->count == rows->capacity)
	{
		rows->capacity = rows->capacity ? rows->capacity * 2 : 4;
		rows->digests = rows->digests ? repalloc_array(rows->digests, PLHash, rows->capacity) :
			palloc_array(PLHash, rows->capacity);
	}
	if (!key_digest(rows->index, key, &rows->digests[rows->count]))
		rows->digests[rows->count] = dg_rowhash((PLSlice) {value.data + 8, value.len - 8});
	rows->count++;
	return PL_OK;
}

/* The digest row |handle| maps to in |s|, and the row |digest| names. */
static bool
handle_digest_of(DGState *s, uint64 handle, PLHash *digest)
{
	return dg_handle_digest(s, handle, digest);
}
static bool
row_of(DGState *s, PLHash digest, PLSlice *row)
{
	PLValue		value;

	dg_check(pl_get(&s->content, (PLSlice) {digest.bytes, PL_HASH_SIZE}, &value));
	if (!value.present || value.bytes.len < 8)
		return false;
	*row = (PLSlice) {value.bytes.data + 8, value.bytes.len - 8};
	return true;
}

/*
 * Another live row of |table| whose |index| key equals that of the row at
 * |tid|, as this transaction sees the latest state; zero if there is none.
 */
static uint64
unique_conflict(Relation table, Relation index, uint64 self, StringInfo prefix)
{
	DGBatch    *rows_batch = dg_batch_current(table),
			   *entries_batch = dg_batch_current(index);

	/* The latest trees, as another inserter of the key may just have committed. */
	DGState    *rows = rows_batch ? dg_batch_load(table) : dg_load(table, SnapshotSelf);
	DGState    *entries = entries_batch ? dg_batch_load(index) : dg_load(index, SnapshotSelf);
	StringInfoData upper;
	PLSlice		lo = {(uint8 *) prefix->data, prefix->len},
				hi;
	KeyRows		found = {index};
	List	   *added = NIL,
			   *removed = NIL;
	ListCell   *lc;
	uint64		conflict = 0;

	initStringInfo(&upper);
	appendBinaryStringInfo(&upper, prefix->data, prefix->len);
	if (!prefix_successor(&upper))
		elog(ERROR, "invalid prolly index key prefix");
	hi.data = (uint8 *) upper.data;
	hi.len = upper.len;
	dg_check(pl_scan(&entries->content, &lo, &hi, collect_key_rows, &found));

	/* The command's own entries, not yet in the trees, count too. */
	if (entries_batch)
		dg_batch_index_prefix(entries_batch, lo, &added, &removed);
	foreach(lc, added)
	{
		PLHash	   *digest = lfirst(lc);

		if (found.count == found.capacity)
		{
			found.capacity = found.capacity ? found.capacity * 2 : 4;
			found.digests = found.digests ? repalloc_array(found.digests, PLHash, found.capacity) :
				palloc_array(PLHash, found.capacity);
		}
		found.digests[found.count++] = *digest;
	}
	for (int i = 0; i < found.count && !conflict; i++)
	{
		HandleList	handles = handles_of(rows, found.digests[i], -1);
		uint64	   *pending;
		int			n = rows_batch ? dg_batch_handles(rows_batch, found.digests[i], &pending) : 0;
		bool		gone = false;

		int			own = 0;

		/* An entry the command removed no longer names a live row. */
		foreach(lc, removed)
			if (pl_hash_equal(*(PLHash *) lfirst(lc), found.digests[i]))
				gone = true;
		for (int j = 0; j < handles.count && !conflict && !gone; j++)
			if (handles.items[j] != self)
				conflict = handles.items[j];
			else
				own++;
		for (int j = 0; j < n && !conflict; j++)
			if (pending[j] != self)
				conflict = pending[j];
			else
				own++;

		/*
		 * A committed row identical to ours derives the same handle: two
		 * occurrences named by our handle are our row and a conflicting one.
		 */
		if (!conflict && own > 1)
			conflict = self;
	}
	dg_release(rows);
	dg_release(entries);
	return conflict;
}

/*
 * Raise a unique violation if another live row has the same key as the row
 * at |tid|, which the table AM already added to |index|.
 *
 * Concurrent inserters of one key serialize on a reservation: a lock record
 * keyed by the index storage and a hash of the key, held until commit. A
 * conflicting row that a running transaction is deleting, or updating with a
 * key change, holds its row lock exclusively; waiting for a key-share lock on
 * it lets that transaction finish first, as heap unique checks wait for the
 * deleting transaction.
 */
bool
dg_index_check_unique(Relation table, Relation index, ItemPointer tid, Datum *values, bool *isnull,
					  IndexUniqueCheck check)
{
	DGBatch    *batch = dg_batch_current(table);
	DGState    *rows = batch ? NULL : dg_load(table, SnapshotSelf);
	uint64		self = dg_id(tid);
	PLHash		digest;
	PLSlice		row;
	StringInfoData key;
	size_t		prefix;
	bool		nullkey;

	/* The row may still be pending in the command's batch. */
	if (batch ? !dg_batch_handle_digest(batch, self, &digest) : !handle_digest_of(rows, self, &digest))
		elog(ERROR, "prolly row to check is missing");
	if (batch ? !dg_batch_row(batch, digest, &row) : !row_of(rows, digest, &row))
		elog(ERROR, "missing canonical row contents");
	initStringInfo(&key);
	if (!index_entry(table, index, row, (PLSlice) {digest.bytes, PL_HASH_SIZE}, &key, &prefix, &nullkey))
		elog(ERROR, "unique check for row outside index predicate");
	if (rows)
		dg_release(rows);
	if (nullkey && !index->rd_index->indnullsnotdistinct)
		return true;
	key.len = prefix;
	return check_key(table, index, self, &key, (PLSlice) {NULL, 0}, check);
}

/*
 * Raise a unique violation if another live row shares |key| (an index key
 * prefix) with row |self|. |row|, if given, describes the key for the error.
 *
 * A partial check instead returns false, and the executor checks again later:
 * for speculative insertion at once, waiting for whoever changes the other
 * row, and for a deferred constraint with an existing check, which waits as
 * an immediate one does. A deferred partial check waits for nothing; a
 * reservation another transaction holds is a potential conflict.
 */
static bool
check_key(Relation table, Relation index, uint64 self, StringInfo key, PLSlice row, IndexUniqueCheck check)
{
	const char *branch = dg_relation_branch(table);
	uint64		conflict;
	TM_FailureData failure;
	int64		next;

	bool		deferred = check == UNIQUE_CHECK_PARTIAL && !index->rd_index->indimmediate;

	if (!dg_key_reserve(index, dg_relation_branch(index),
						hash_bytes_extended((const unsigned char *) key->data, key->len, 0), !deferred))
		return false;
	while ((conflict = unique_conflict(table, index, self, key)) != 0)
	{
		CommandId	cid;
		TupleTableSlot *slot;
		ItemPointerData tid;
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		DGState    *rows;
		PLHash		digest;
		PLValue		found;

		if (check == UNIQUE_CHECK_PARTIAL)
			return false;
		if (conflict != self && !dg_row_modified(table, branch, conflict, &cid))
		{
			dg_record_lock(table, branch, (int64) conflict, GetCurrentCommandId(true), LockTupleKeyShare, LockWaitBlock, false, &failure, &next);
			if (unique_conflict(table, index, self, key) != conflict)
				continue;
		}

		/*
		 * As _bt_check_unique, give serializable isolation the chance to
		 * report a conflict the unique violation would otherwise mask.
		 */
		dg_serializable_write(table);

		/* Describe the key from our row, which the error reports. */
		rows = dg_load(table, SnapshotSelf);
		if (!dg_handle_digest(rows, self, &digest))
			elog(ERROR, "prolly row of a unique check is missing");
		dg_check(pl_get(&rows->content, (PLSlice) {digest.bytes, PL_HASH_SIZE}, &found));
		slot = table_slot_create(table, NULL);
		dg_tid(self, &tid);
		if (row.data)
			dg_decode(table, row, &tid, slot);
		else
			dg_decode(table, (PLSlice) {found.bytes.data + 8, found.bytes.len - 8}, &tid, slot);
		index_values(index, slot, values, isnull);
		unique_violation(table, index, values, isnull);
	}
	return true;
}

static void
unique_violation(Relation table, Relation index, Datum *values, bool *isnull)
{
	char	   *key = BuildIndexValueDescription(index, values, isnull);

	ereport(ERROR,
			(errcode(ERRCODE_UNIQUE_VIOLATION),
			 errmsg("duplicate key value violates unique constraint \"%s\"", RelationGetRelationName(index)),
			 key ? errdetail("Key %s already exists.", key) : 0,
			 errtableconstraint(table, RelationGetRelationName(index))));
}
static IndexBuildResult *
build(Relation table, Relation index, IndexInfo *info)
{
	IndexBuildResult *result = palloc0(sizeof(*result));
	DGState    *s;

	if (info->ii_Concurrent)
		dg_unsupported("concurrent index builds");
	dg_index_rebuild(table, index);
	s = dg_load(table, SnapshotSelf);
	result->heap_tuples = result->index_tuples = dg_handle_count(s);
	dg_release(s);
	return result;
}
static void
build_empty(Relation rel)
{
	dg_unsupported("unlogged indexes");
}
static bool
insert(Relation index, Datum *values, bool *nulls, ItemPointer tid, Relation table, IndexUniqueCheck unique, bool unchanged, IndexInfo *info)
{
	/*
	 * The table AM added the entry, and checks bound storage itself as it
	 * writes. An existing check, a deferred constraint's recheck, finds the
	 * entry there, and may run against bound storage.
	 */
	if (unique == UNIQUE_CHECK_EXISTING || (unique != UNIQUE_CHECK_NO && !dg_storage_bound(table)))
		return dg_index_check_unique(table, index, tid, values, nulls, unique);
	return true;
}
static IndexBulkDeleteResult *
bulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats, IndexBulkDeleteCallback cb, void *arg)
{
	dg_unsupported("index vacuum");
}
static IndexBulkDeleteResult *
vacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	return stats ? stats : palloc0(sizeof(*stats));
}
/*
 * The rows a scan of |path| reads, as btcostestimate counts them: a scan
 * reads every entry within its bounds, which come from equality quals on
 * leading key columns and at most one range column after them, and a unique
 * index bound by equality on every key column reads one. Without statistics,
 * which branch tables do not have yet, a partly bound scan's estimate can be
 * as small as a fully bound one's, and every estimate below one row rounds up
 * to it; counting one row more than the bound quals select keeps both orders
 * right: a unique lookup beats any other scan, and a scan bound on more
 * columns beats one bound on fewer.
 */
static double
bounded_rows(PlannerInfo *root, IndexPath *path)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *bound = NIL;
	int			column = 0;
	bool		equal_here = false,
				all_equal = true;
	ListCell   *lc;
	double		rows;

	foreach(lc, path->indexclauses)
	{
		IndexClause *clause = lfirst_node(IndexClause, lc);
		ListCell   *lq;

		if (clause->indexcol != column)
		{
			if (!equal_here || clause->indexcol != column + 1)
				break;
			column = clause->indexcol;
			equal_here = false;
		}
		foreach(lq, clause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lq);

			if (IsA(rinfo->clause, OpExpr) &&
				get_op_opfamily_strategy(((OpExpr *) rinfo->clause)->opno, index->opfamily[column]) == BTEqualStrategyNumber)
				equal_here = true;
			else
				all_equal = false;
			bound = lappend(bound, rinfo);
		}
	}
	if (index->unique && equal_here && all_equal && column == index->nkeycolumns - 1)
		return 1.0;
	rows = clauselist_selectivity(root, bound, index->rel->relid, JOIN_INNER, NULL) * index->rel->tuples;
	return 1.0 + rows;
}

static void
cost(PlannerInfo *root, IndexPath *path, double loops, Cost *startup, Cost *total, Selectivity *selectivity, double *correlation, double *pages)
{
	GenericCosts costs = {0};

	costs.numIndexTuples = bounded_rows(root, path);
	genericcostestimate(root, path, loops, &costs);
	*startup = costs.indexStartupCost;
	*total = costs.indexTotalCost;
	*selectivity = costs.indexSelectivity;
	*correlation = 0;
	*pages = costs.numIndexPages;
}
static bytea *
options(Datum reloptions, bool validate)
{
	if (DatumGetPointer(reloptions))
		dg_unsupported("index storage options");
	return NULL;
}
static bool
validate(Oid opclass)
{
	return true;
}
static IndexScanDesc
beginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(rel, nkeys, norderbys);
	DGIndexScan *s = palloc0(sizeof(*s));

	if (norderbys)
		dg_unsupported("ORDER BY operators");
	s->context = AllocSetContextCreate(CurrentMemoryContext, "prolly index scan", ALLOCSET_DEFAULT_SIZES);
	s->position = -1;
	scan->opaque = s;
	return scan;
}
static bool
matches(IndexScanDesc scan, TupleTableSlot *slot)
{
	int			i;
	Datum values[INDEX_MAX_KEYS];
	bool nulls[INDEX_MAX_KEYS];

	index_values(scan->indexRelation, slot, values, nulls);
	for (i = 0; i < scan->numberOfKeys; i++)
	{
		ScanKey		key = &scan->keyData[i];
		bool		null = nulls[key->sk_attno - 1];
		Datum		value = values[key->sk_attno - 1];

		if (key->sk_flags & SK_SEARCHNULL)
		{
			if (!null)
				return false;
		}
		else if (key->sk_flags & SK_SEARCHNOTNULL)
		{
			if (null)
				return false;
		}
		else
		{
			if (key->sk_flags & ~SK_ISNULL)
				dg_unsupported("this index scan-key flag");
			if (null || key->sk_flags & SK_ISNULL || !DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation, value, key->sk_argument)))
				return false;
		}
	}
	return true;
}
/* Append column |j|'s tag and escaped canonical bytes for scan key |key|. */
static void
append_key_value(IndexScanDesc scan, int j, ScanKey key, StringInfo out)
{
	Relation	index = scan->indexRelation;
	Oid			type = OidIsValid(key->sk_subtype) && key->sk_subtype != ANYENUMOID && key->sk_subtype != ANYARRAYOID &&
		key->sk_subtype != RECORDOID && key->sk_subtype != ANYRANGEOID && key->sk_subtype != ANYMULTIRANGEOID ? key->sk_subtype :
		TupleDescAttr(RelationGetDescr(index), j)->atttypid;

	appendStringInfoChar(out, 1);
	escaped(out, dg_key_value(type, key->sk_argument), (index->rd_indoption[j] & INDOPTION_DESC) != 0);
}

/*
 * Datetime cross-type operators compare after unit/time-zone conversion.
 * Their independently encoded operands are not comparable byte bounds: dates
 * count days, timestamps count microseconds, and timestamptz uses UTC. Leave
 * these predicates to matches(), retaining any usable leading-column bounds.
 */
static bool
key_has_compatible_encoding(IndexScanDesc scan, int j, ScanKey key)
{
	Oid			column = getBaseType(TupleDescAttr(RelationGetDescr(scan->indexRelation), j)->atttypid);
	Oid			argument = OidIsValid(key->sk_subtype) ? getBaseType(key->sk_subtype) : column;

	if (column != argument &&
		(column == DATEOID || column == TIMESTAMPOID || column == TIMESTAMPTZOID))
		return false;
	return true;
}

/*
 * Conservative key bounds for the scan: equality keys on leading columns,
 * then at most one range column. matches() rechecks every candidate, so the
 * bounds only need to contain every match.
 */
static void
scan_bounds(IndexScanDesc scan, StringInfo lower, StringInfo upper, bool *has_lower, bool *has_upper)
{
	Relation	index = scan->indexRelation;
	StringInfoData prefix;

	initStringInfo(&prefix);
	*has_lower = *has_upper = false;
	for (int j = 0; j < index->rd_index->indnkeyatts; j++)
	{
		ScanKey		equal = NULL,
					low = NULL,
					high = NULL;
		bool		desc = (index->rd_indoption[j] & INDOPTION_DESC) != 0;

		for (int i = 0; i < scan->numberOfKeys; i++)
		{
			ScanKey		key = &scan->keyData[i];

			if (key->sk_attno != j + 1 || key->sk_flags != 0 ||
				!key_has_compatible_encoding(scan, j, key))
				continue;
			if (key->sk_strategy == BTEqualStrategyNumber)
				equal = key;
			else if (key->sk_strategy == BTGreaterStrategyNumber || key->sk_strategy == BTGreaterEqualStrategyNumber)
				low = key;
			else if (key->sk_strategy == BTLessStrategyNumber || key->sk_strategy == BTLessEqualStrategyNumber)
				high = key;
		}
		if (equal)
		{
			append_key_value(scan, j, equal, &prefix);
			continue;
		}
		if (desc)
		{
			ScanKey		swap = low;

			low = high;
			high = swap;
		}
		resetStringInfo(lower);
		appendBinaryStringInfo(lower, prefix.data, prefix.len);
		if (low)
			append_key_value(scan, j, low, lower);
		*has_lower = lower->len > 0;
		resetStringInfo(upper);
		appendBinaryStringInfo(upper, prefix.data, prefix.len);
		if (high)
			append_key_value(scan, j, high, upper);
		*has_upper = prefix_successor(upper);
		return;
	}
	resetStringInfo(lower);
	appendBinaryStringInfo(lower, prefix.data, prefix.len);
	*has_lower = lower->len > 0;
	resetStringInfo(upper);
	appendBinaryStringInfo(upper, prefix.data, prefix.len);
	*has_upper = prefix_successor(upper);
}

/*
 * Under a dirty snapshot, wait out any other transaction holding the unique-key
 * reservation the scan's equality keys name. A competing inserter's row is
 * private until it commits, so where heap's dirty snapshot would report that
 * inserter, this waits for it before reading the latest rows.
 */
static void
wait_for_reservation(IndexScanDesc scan)
{
	Relation	index = scan->indexRelation;
	StringInfoData key;
	int64		id;
	TransactionId xwait;

	if (!index->rd_index->indisunique || dg_storage_bound(scan->heapRelation))
		return;
	initStringInfo(&key);
	for (int j = 0; j < index->rd_index->indnkeyatts; j++)
	{
		ScanKey		equal = NULL;

		for (int i = 0; i < scan->numberOfKeys && !equal; i++)
			if (scan->keyData[i].sk_attno == j + 1 && scan->keyData[i].sk_flags == 0 &&
				scan->keyData[i].sk_strategy == BTEqualStrategyNumber)
				equal = &scan->keyData[i];
		if (!equal)
			return;
		append_key_value(scan, j, equal, &key);
	}
	id = (int64) hash_bytes_extended((const unsigned char *) key.data, key.len, 0);
	while (TransactionIdIsValid(xwait = dg_key_reserver(index, dg_relation_branch(index), (uint64) id)))
		XactLockTableWait(xwait, NULL, NULL, XLTW_None);
}

static void
drop_slot(DGIndexScan *s)
{
	if (s->slot)
		ExecDropSingleTupleTableSlot(s->slot);
	s->slot = NULL;
}

static void
rescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
	DGIndexScan *s = scan->opaque;
	MemoryContext old;

	/* Bitmap scans do not supply heapRelation; the parent holds its lock.
	 * indrelid is the revision-bound relation handle, not a storage OID.
	 */
	if (!scan->heapRelation)
	{
		s->bitmap_table = table_open(scan->indexRelation->rd_index->indrelid, NoLock);
		scan->heapRelation = s->bitmap_table;
	}
	if (keys && nkeys)
		memcpy(scan->keyData, keys, sizeof(ScanKeyData) * nkeys);
	drop_slot(s);
	MemoryContextReset(s->context);
	old = MemoryContextSwitchTo(s->context);
	if (scan->xs_snapshot->snapshot_type == SNAPSHOT_DIRTY)
		wait_for_reservation(scan);
	s->index = dg_load(scan->indexRelation, scan->xs_snapshot);
	s->table = dg_load(scan->heapRelation, scan->xs_snapshot);
	pgstat_count_index_scan(scan->indexRelation);
	if (scan->instrument)
		scan->instrument->nsearches++;
	initStringInfo(&s->lower);
	initStringInfo(&s->upper);
	scan_bounds(scan, &s->lower, &s->upper, &s->has_lower, &s->has_upper);
	s->slot = table_slot_create(scan->heapRelation, NULL);
	s->started = s->exhausted = false;
	s->capacity = 16;
	s->tids = palloc(s->capacity * sizeof(ItemPointerData));
	s->tuples = palloc0(s->capacity * sizeof(HeapTuple));
	s->count = 0;
	s->position = -1;
	s->mark = -1;
	MemoryContextSwitchTo(old);
}

/* Position the cursor on the next entry in the scan's direction, if any is in bounds. */
static void
step(DGIndexScan *s)
{
	PLSlice		lo = {(uint8 *) s->lower.data, s->lower.len},
				hi = {(uint8 *) s->upper.data, s->upper.len};
	PLEntry		entry;

	if (!s->started)
	{
		s->started = true;
		if (s->backward)
			dg_check(pl_cursor_init_last(&s->cursor, &s->index->content, s->has_upper ? &hi : NULL));
		else
			dg_check(pl_cursor_init(&s->cursor, &s->index->content, s->has_lower ? &lo : NULL));
	}
	else if (s->backward)
		dg_check(pl_cursor_prev(&s->cursor));
	else
		dg_check(pl_cursor_next(&s->cursor));
	if (!s->cursor.valid)
	{
		s->exhausted = true;
		return;
	}
	entry = pl_cursor_entry(&s->cursor);
	if (s->backward ? s->has_lower && pl_compare(entry.key, lo) < 0 :
		s->has_upper && pl_compare(entry.key, hi) >= 0)
		s->exhausted = true;
}

static void
add_item(DGIndexScan *s, ItemPointer tid, HeapTuple tuple)
{
	if (s->count == s->capacity)
	{
		s->capacity *= 2;
		s->tids = repalloc(s->tids, s->capacity * sizeof(ItemPointerData));
		s->tuples = repalloc0(s->tuples, s->count * sizeof(HeapTuple), s->capacity * sizeof(HeapTuple));
	}
	s->tids[s->count] = *tid;
	s->tuples[s->count++] = tuple;
}

/*
 * Read index entries until one yields items or the scan's bounds are
 * reached. An index-only scan returns entries, not rows: it needs no
 * handles, only each entry's number of occurrences, and its TIDs are never
 * fetched. Other scans find the rows' handles, which name the TIDs: derived
 * from each entry's digest, plus any allocated ones the delta holds for it.
 */
static void
produce(IndexScanDesc scan)
{
	DGIndexScan *s = scan->opaque;
	MemoryContext old = MemoryContextSwitchTo(s->context);
	size_t		before = s->count;

	while (s->count == before && !s->exhausted)
	{
		PLEntry		entry;
		PLSlice		row;
		HandleList	handles;
		ItemPointerData first_tid;

		CHECK_FOR_INTERRUPTS();
		step(s);
		if (s->exhausted)
			break;
		entry = pl_cursor_entry(&s->cursor);
		row = (PLSlice) {entry.value.data + 8, entry.value.len - 8};
		if (scan->xs_want_itup)
		{
			handles.count = (int) Min(pl_read64(entry.value.data), (uint64) INT_MAX);
			handles.items = NULL;
		}
		else
		{
			PLHash		digest;

			if (!key_digest(scan->indexRelation, entry.key, &digest))
				digest = dg_rowhash(row);
			handles = handles_of(s->table, digest, (int64) pl_read64(entry.value.data));
		}

		/* Duplicate occurrences share bytes and qualifications, not TIDs. */
		if (handles.count == 0)
			continue;

		/* Bitmap bounds are conservative. Defer the qualification to the
		 * table scan so that it decodes each candidate only once, even when
		 * several index searches select the same occurrence.
		 */
		if (handles.items)
			dg_tid(handles.items[0], &first_tid);
		else
			ItemPointerSet(&first_tid, 0, FirstOffsetNumber);
		if (!s->bitmap_table)
		{
			dg_decode(scan->heapRelation, row, &first_tid, s->slot);
			if (!matches(scan, s->slot))
				continue;
		}
		for (int j = 0; j < handles.count; j++)
		{
			ItemPointerData tid = first_tid;
			HeapTuple	tuple = NULL;

			if (handles.items)
			{
				dg_tid(handles.items[j], &tid);
				dg_stash_put(RelationGetRelid(scan->heapRelation), s->table, handles.items[j], row);
			}
			if (scan->xs_want_itup)
			{
				Datum		values[INDEX_MAX_KEYS];
				bool		nulls[INDEX_MAX_KEYS];

				index_values(scan->indexRelation, s->slot, values, nulls);
				tuple = heap_form_tuple(RelationGetDescr(scan->indexRelation), values, nulls);
			}
			add_item(s, &tid, tuple);
		}
	}
	MemoryContextSwitchTo(old);
}

static bool
gettuple(IndexScanDesc scan, ScanDirection dir)
{
	DGIndexScan *s = scan->opaque;
	bool		backward = ScanDirectionIsBackward(dir);

	if (!s->started)
		s->backward = backward;
	if (backward == s->backward)
	{
		if (s->position < (int64) s->count)
			s->position++;
		while (s->position >= (int64) s->count && !s->exhausted)
			produce(scan);
		if (s->position >= (int64) s->count)
			return false;
	}
	else
	{
		if (s->position >= 0)
			s->position--;
		if (s->position < 0)
			return false;
	}
	scan->xs_heaptid = s->tids[s->position];
	scan->xs_recheck = false;
	scan->xs_hitup = s->tuples[s->position];
	scan->xs_hitupdesc = RelationGetDescr(scan->indexRelation);
	return true;
}
/* The executor expands array keys and unions/intersects bitmap results. */
static int64
getbitmap(IndexScanDesc scan, TIDBitmap *bitmap)
{
	DGIndexScan *s = scan->opaque;

	while (!s->exhausted)
		produce(scan);
	for (size_t i = 0; i < s->count; i++)
	{
		CHECK_FOR_INTERRUPTS();
		tbm_add_tuples(bitmap, &s->tids[i], 1, true);
	}
	return s->count;
}
static void
endscan(IndexScanDesc scan)
{
	DGIndexScan *s = scan->opaque;

	drop_slot(s);
	if (s->bitmap_table)
		table_close(s->bitmap_table, NoLock);
	MemoryContextDelete(s->context);
	pfree(s);
}
static void
markpos(IndexScanDesc scan)
{
	DGIndexScan *s = scan->opaque;

	s->mark = s->position;
}
static void
restorepos(IndexScanDesc scan)
{
	DGIndexScan *s = scan->opaque;

	s->position = s->mark;
}
static bool
canreturn(Relation index, int attribute)
{
	return true;
}
Datum
dolt_prolly_index_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = makeNode(IndexAmRoutine);

	am->amstrategies = 5;
	am->amsupport = 1;
	am->amcanorder = true;
	am->amcanbackward = true;
	am->amcanunique = true;
	am->amcanmulticol = true;
	am->amoptionalkey = true;
	am->amsearchnulls = true;
	am->amcaninclude = true;
	am->amconsistentequality = true;
	am->amconsistentordering = true;
	am->ambuild = build;
	am->ambuildempty = build_empty;
	am->aminsert = insert;
	am->ambulkdelete = bulkdelete;
	am->amvacuumcleanup = vacuumcleanup;
	am->amcostestimate = cost;
	am->amcanreturn = canreturn;
	am->amoptions = options;
	am->amvalidate = validate;
	am->ambeginscan = beginscan;
	am->amrescan = rescan;
	am->amgettuple = gettuple;
	am->amgetbitmap = getbitmap;
	am->amendscan = endscan;
	am->ammarkpos = markpos;
	am->amrestrpos = restorepos;
	am->amtranslatestrategy = bttranslatestrategy;
	am->amtranslatecmptype = bttranslatecmptype;
	PG_RETURN_POINTER(am);
}
