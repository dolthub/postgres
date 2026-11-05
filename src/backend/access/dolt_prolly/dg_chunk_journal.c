/*-------------------------------------------------------------------------
 *
 * dg_chunk_journal.c
 *	  The chunk store: an append-only journal of chunk records with sorted
 *	  index runs, outside the buffer manager and, for derivable chunks,
 *	  outside the WAL.
 *
 * One journal serves the cluster; chunks are content addressed, so
 * databases share it and CREATE DATABASE copies nothing. Two plain files:
 *
 *	- pg_dolt/chunk_journal is a stream of records: a header (CRC-32C,
 *	  length, kind, hash) and the chunk's bytes. A tombstone is a header
 *	  alone.
 *
 *	- pg_dolt/chunk_index holds two metapages, in blocks 0 and 1, and the
 *	  sorted runs after them. The newer valid metapage names the stream
 *	  position the runs cover and the live runs, oldest first. A run is a
 *	  sequence of pages of (hash, position) entries in hash order, followed by
 *	  fence pages listing each page's first hash.
 *
 * Records past the indexed position are indexed in shared memory, in an
 * open-addressing table. When it fills, its entries become a new run, and
 * runs merge as a binary counter does, so an entry is rewritten about
 * log2(runs) times. Run blocks are never reused, so a reader may finish
 * reading a run that a merge replaced.
 *
 * Appends are plain writes, serialized by DoltChunkAppend, and a record is
 * written before it is indexed: a reader who finds an entry can read its
 * record. Readers hold DoltChunkIndex shared only while they look in the
 * recent index and copy the run list, and read records and runs without it.
 *
 * Durability. A logged put also writes a DoltChunk PUT record carrying the
 * chunks, after appending them, so a checkpoint whose redo point follows the
 * record finds them appended and fsyncs them; redo appends whatever the
 * journal lacks. The stream is fsynced up to the indexed position before a
 * metapage moves, and runs before the metapage naming them. At startup, before
 * redo, the stream past the indexed position is indexed until a record fails
 * its CRC, and truncated there.
 *
 * src/backend/access/dolt_prolly/dg_chunk_journal.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/doltchunk.h"
#include "access/doltchunk_xlog.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "miscadmin.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/memutils.h"
#include "utils/wait_event.h"

#define JR_DIR				"pg_dolt"
#define JR_DATA_PATH		JR_DIR "/chunk_journal"
#define JR_INDEX_PATH		JR_DIR "/chunk_index"

#define JR_MAGIC			0x444A524E	/* "DJRN" */
#define JR_KIND_CHUNK		1
#define JR_KIND_TOMBSTONE	2
#define JR_KIND_UNLOGGED	3		/* a chunk no WAL record carries */

#define JR_MAX_RUNS			48
#define JR_META_SLOTS		2		/* blocks 0 and 1 */

/* The largest batch of chunk bytes one PUT record carries. */
#define JR_RECORD_BYTES		(4 * 1024 * 1024)

/* The largest chunk: well within one WAL record. */
#define JR_MAX_CHUNK		(256 * 1024 * 1024)

#define JR_TOMBSTONE_BIT	(UINT64CONST(1) << 62)
#define JR_UNLOGGED_BIT		(UINT64CONST(1) << 61)
#define JR_POSITION(located)	(((located) & ~(JR_TOMBSTONE_BIT | JR_UNLOGGED_BIT)) - 1)

int			dolt_chunk_journal_tail = 131072;

typedef struct JrRecordHeader
{
	pg_crc32c	crc;			/* over the rest of the header and the data */
	uint32		length;
	uint32		kind;
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
} JrRecordHeader;

/*
 * An index entry: the stream position of a hash's latest record, plus one so
 * that 0 marks an empty slot, with JR_TOMBSTONE_BIT if that record deletes it
 * and JR_UNLOGGED_BIT if no WAL record carries it.
 */
typedef struct JrEntry
{
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
	uint32		pad;
	uint64		position;
} JrEntry;

#define JR_PAGE_HEADER		(2 * sizeof(uint32))
#define JR_RUN_ENTRIES		((BLCKSZ - JR_PAGE_HEADER) / sizeof(JrEntry))
#define JR_FENCES_PER_PAGE	(BLCKSZ / DOLT_CHUNK_HASH_SIZE)

typedef struct JrRun
{
	uint64		start;			/* first block */
	uint32		pages;
	uint32		fence_pages;	/* after the entry pages */
	uint64		count;
} JrRun;

typedef struct JrMeta
{
	uint32		magic;
	pg_crc32c	crc;			/* over the rest of the metapage */
	uint64		generation;
	uint64		indexed;		/* stream position the runs cover */
	uint64		next_free;		/* next free block */
	uint32		nruns;
	uint32		pad;
	JrRun		runs[JR_MAX_RUNS];	/* oldest first */
} JrMeta;

typedef struct JrShared
{
	LWLock		append_lock;	/* appends, run writes, merges and loading */
	LWLock		index_lock;		/* the recent index and the run list */
	bool		loaded;
	uint64		end;			/* stream position of the next record */
	pg_atomic_uint64 synced;	/* the stream is durable up to here */
	uint64		indexed;
	uint64		next_free;
	uint64		generation;
	uint32		nruns;
	JrRun		runs[JR_MAX_RUNS];
	uint32		capacity;		/* slots in the recent index */
	uint32		tail_count;
	JrEntry		tail[FLEXIBLE_ARRAY_MEMBER];
} JrShared;

/* A run's fences, read once per process; run blocks never change. */
typedef struct JrFences
{
	uint64		start;			/* hash key */
	uint32		pages;
	uint8	   *first;			/* pages * DOLT_CHUNK_HASH_SIZE */
} JrFences;

typedef struct JrState
{
	DoltChunkIntent intent;
} JrState;

static JrShared *shared;
static File data_file = -1;
static File index_file = -1;
static HTAB *fences;

/* ---- Shared memory ---- */

static uint32
tail_capacity(void)
{
	return (uint32) Max(dolt_chunk_journal_tail, 64) * 2;
}

Size
DoltChunkJournalShmemSize(void)
{
	return add_size(offsetof(JrShared, tail), mul_size(tail_capacity(), sizeof(JrEntry)));
}

void
DoltChunkJournalShmemInit(void)
{
	bool		found;

	shared = ShmemInitStruct("dolt chunk journal", DoltChunkJournalShmemSize(), &found);
	if (found)
		return;
	memset(shared, 0, DoltChunkJournalShmemSize());
	LWLockInitialize(&shared->append_lock, LWTRANCHE_DOLT_CHUNK_APPEND);
	LWLockInitialize(&shared->index_lock, LWTRANCHE_DOLT_CHUNK_INDEX);
	pg_atomic_init_u64(&shared->synced, 0);
	shared->capacity = tail_capacity();
}

/* ---- Files ---- */

static void
open_files(void)
{
	if (data_file >= 0)
		return;
	if (MakePGDirectory(JR_DIR) < 0 && errno != EEXIST)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not create directory \"%s\": %m", JR_DIR)));
	data_file = PathNameOpenFile(JR_DATA_PATH, O_RDWR | O_CREAT | PG_BINARY);
	if (data_file < 0)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not open file \"%s\": %m", JR_DATA_PATH)));
	index_file = PathNameOpenFile(JR_INDEX_PATH, O_RDWR | O_CREAT | PG_BINARY);
	if (index_file < 0)
		ereport(ERROR, (errcode_for_file_access(), errmsg("could not open file \"%s\": %m", JR_INDEX_PATH)));
}

static void
file_read(File file, void *target, Size length, uint64 offset, const char *path)
{
	ssize_t		got = FileRead(file, target, length, (off_t) offset, WAIT_EVENT_DOLT_CHUNK_READ);

	if (got != (ssize_t) length)
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("could not read %zu bytes at %llu of file \"%s\": %m",
							   length, (unsigned long long) offset, path)));
}

static void
file_write(File file, const void *source, Size length, uint64 offset, const char *path)
{
	ssize_t		put = FileWrite(file, source, length, (off_t) offset, WAIT_EVENT_DOLT_CHUNK_WRITE);

	if (put != (ssize_t) length)
		ereport(ERROR, (errcode_for_file_access(),
						errmsg("could not write %zu bytes at %llu of file \"%s\": %m",
							   length, (unsigned long long) offset, path)));
}

static void
file_sync(File file, const char *path)
{
	if (FileSync(file, WAIT_EVENT_DOLT_CHUNK_SYNC) != 0)
		ereport(data_sync_elevel(ERROR), (errcode_for_file_access(), errmsg("could not fsync file \"%s\": %m", path)));
}

/* Make the stream durable up to |upto|: every append below it has been written. */
static void
stream_sync(uint64 upto)
{
	uint64		synced = pg_atomic_read_u64(&shared->synced);

	if (synced >= upto)
		return;
	file_sync(data_file, JR_DATA_PATH);
	while (synced < upto && !pg_atomic_compare_exchange_u64(&shared->synced, &synced, upto))
		;
}

static pg_crc32c
meta_crc(const JrMeta *meta)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (const char *) meta + offsetof(JrMeta, generation), sizeof(JrMeta) - offsetof(JrMeta, generation));
	FIN_CRC32C(crc);
	return crc;
}

/*
 * Write the next metapage, into the slot the current one doesn't use, once
 * the stream it covers and the runs it names are durable. Under append_lock.
 */
static void
write_meta(void)
{
	PGAlignedBlock block = {0};
	JrMeta	   *meta = (JrMeta *) block.data;

	stream_sync(shared->indexed);
	file_sync(index_file, JR_INDEX_PATH);
	shared->generation++;
	meta->magic = JR_MAGIC;
	meta->generation = shared->generation;
	meta->indexed = shared->indexed;
	meta->next_free = shared->next_free;
	meta->nruns = shared->nruns;
	memcpy(meta->runs, shared->runs, sizeof(meta->runs));
	meta->crc = meta_crc(meta);
	file_write(index_file, block.data, BLCKSZ, (shared->generation % JR_META_SLOTS) * BLCKSZ, JR_INDEX_PATH);
	file_sync(index_file, JR_INDEX_PATH);
}

/* ---- Records ---- */

static pg_crc32c
record_crc(const JrRecordHeader *header, const uint8 *data)
{
	pg_crc32c	crc;

	INIT_CRC32C(crc);
	COMP_CRC32C(crc, (const char *) header + sizeof(pg_crc32c), sizeof(JrRecordHeader) - sizeof(pg_crc32c));
	if (header->length)
		COMP_CRC32C(crc, data, header->length);
	FIN_CRC32C(crc);
	return crc;
}

/* ---- The recent index ---- */

static uint32
tail_slot(const uint8 *hash)
{
	uint64		h;

	memcpy(&h, hash, sizeof(h));
	return (uint32) (h % shared->capacity);
}

static JrEntry *
tail_find(const uint8 *hash)
{
	for (uint32 i = tail_slot(hash);; i = (i + 1) % shared->capacity)
	{
		JrEntry    *entry = &shared->tail[i];

		if (entry->position == 0)
			return NULL;
		if (memcmp(entry->hash, hash, DOLT_CHUNK_HASH_SIZE) == 0)
			return entry;
	}
}

/* Under index_lock, exclusive. */
static void
tail_set(const uint8 *hash, uint64 position)
{
	for (uint32 i = tail_slot(hash);; i = (i + 1) % shared->capacity)
	{
		JrEntry    *entry = &shared->tail[i];

		if (entry->position == 0)
		{
			memcpy(entry->hash, hash, DOLT_CHUNK_HASH_SIZE);
			shared->tail_count++;
		}
		else if (memcmp(entry->hash, hash, DOLT_CHUNK_HASH_SIZE) != 0)
			continue;
		entry->position = position;
		return;
	}
}

static bool
tail_full(void)
{
	return shared->tail_count >= shared->capacity / 2;
}

/* ---- Runs ---- */

static int
compare_entries(const void *a, const void *b)
{
	return memcmp(((const JrEntry *) a)->hash, ((const JrEntry *) b)->hash, DOLT_CHUNK_HASH_SIZE);
}

static JrFences *
run_fences(const JrRun *run)
{
	bool		found;
	JrFences   *f;

	if (!fences)
	{
		HASHCTL		ctl = {0};

		ctl.keysize = sizeof(uint64);
		ctl.entrysize = sizeof(JrFences);
		ctl.hcxt = TopMemoryContext;
		fences = hash_create("dolt chunk journal runs", 32, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	f = hash_search(fences, &run->start, HASH_FIND, NULL);
	if (f)
		return f;

	/* Read the fences before entering them, so an error leaves no empty entry. */
	{
		uint8	   *first = MemoryContextAllocHuge(TopMemoryContext, (Size) Max(run->fence_pages, 1) * BLCKSZ);

		if (run->fence_pages)
			file_read(index_file, first, (Size) run->fence_pages * BLCKSZ, (run->start + run->pages) * BLCKSZ,
					  JR_INDEX_PATH);
		f = hash_search(fences, &run->start, HASH_ENTER, &found);
		f->pages = run->pages;
		f->first = first;
	}
	return f;
}

/* |hash|'s entry in |run|, encoded as in the recent index, or 0. */
static uint64
run_find(const JrRun *run, const uint8 *hash)
{
	JrFences   *f = run_fences(run);
	int			lo = 0,
				hi = (int) f->pages - 1,
				page = -1;
	PGAlignedBlock block;
	uint32		count;
	JrEntry    *entries;

	/* The last page whose first hash is not above |hash|. */
	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;

		if (memcmp(f->first + (Size) mid * DOLT_CHUNK_HASH_SIZE, hash, DOLT_CHUNK_HASH_SIZE) <= 0)
		{
			page = mid;
			lo = mid + 1;
		}
		else
			hi = mid - 1;
	}
	if (page < 0)
		return 0;
	file_read(index_file, block.data, BLCKSZ, (run->start + page) * BLCKSZ, JR_INDEX_PATH);
	memcpy(&count, block.data, sizeof(count));
	entries = (JrEntry *) (block.data + JR_PAGE_HEADER);
	lo = 0;
	hi = (int) count - 1;
	while (lo <= hi)
	{
		int			mid = (lo + hi) / 2;
		int			c = memcmp(entries[mid].hash, hash, DOLT_CHUNK_HASH_SIZE);

		if (c == 0)
			return entries[mid].position;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0;
}

/* Writes a run's pages at the next free blocks, then its fences. Under append_lock. */
typedef struct RunWriter
{
	JrRun		run;
	PGAlignedBlock page;
	uint32		count;			/* entries on the page being filled */
	StringInfoData first;		/* each page's first hash */
} RunWriter;

static void
run_writer_start(RunWriter *w)
{
	memset(&w->run, 0, sizeof(w->run));
	w->run.start = shared->next_free;
	w->count = 0;
	initStringInfo(&w->first);
}

static void
run_writer_page(RunWriter *w)
{
	memcpy(w->page.data, &w->count, sizeof(w->count));
	file_write(index_file, w->page.data, BLCKSZ, (w->run.start + w->run.pages) * BLCKSZ, JR_INDEX_PATH);
	w->run.pages++;
	w->count = 0;
}

static void
run_writer_add(RunWriter *w, const JrEntry *entry)
{
	if (w->count == 0)
	{
		memset(w->page.data, 0, BLCKSZ);
		appendBinaryStringInfo(&w->first, (const char *) entry->hash, DOLT_CHUNK_HASH_SIZE);
	}
	memcpy(w->page.data + JR_PAGE_HEADER + w->count * sizeof(JrEntry), entry, sizeof(JrEntry));
	w->count++;
	w->run.count++;
	if (w->count == JR_RUN_ENTRIES)
		run_writer_page(w);
}

static JrRun
run_writer_finish(RunWriter *w)
{
	Size		fence_bytes;

	if (w->count)
		run_writer_page(w);
	fence_bytes = (Size) w->first.len;
	w->run.fence_pages = (uint32) ((fence_bytes + BLCKSZ - 1) / BLCKSZ);
	if (w->run.fence_pages)
	{
		Size		padded = (Size) w->run.fence_pages * BLCKSZ;

		enlargeStringInfo(&w->first, (int) (padded - fence_bytes));
		memset(w->first.data + fence_bytes, 0, padded - fence_bytes);
		file_write(index_file, w->first.data, padded, (w->run.start + w->run.pages) * BLCKSZ, JR_INDEX_PATH);
	}
	pfree(w->first.data);
	shared->next_free = w->run.start + w->run.pages + w->run.fence_pages;
	return w->run;
}

/* Reads a run's entries in order, a page at a time. */
typedef struct RunReader
{
	const JrRun *run;
	uint32		page;			/* next page to read */
	uint32		count;
	uint32		next;
	PGAlignedBlock block;
} RunReader;

static const JrEntry *
run_reader_next(RunReader *r)
{
	while (r->next == r->count)
	{
		if (r->page == r->run->pages)
			return NULL;
		file_read(index_file, r->block.data, BLCKSZ, (r->run->start + r->page) * BLCKSZ, JR_INDEX_PATH);
		memcpy(&r->count, r->block.data, sizeof(r->count));
		r->next = 0;
		r->page++;
	}
	return (const JrEntry *) (r->block.data + JR_PAGE_HEADER) + r->next++;
}

/*
 * Merge the two newest runs into one: the newer entry for a hash wins, and
 * tombstones go once the oldest run takes part, since nothing older remains
 * for them to shadow. Under append_lock.
 */
static JrRun
runs_merge_pair(const JrRun *older, const JrRun *newer, bool oldest)
{
	RunReader  *a = palloc0(sizeof(RunReader)),
			   *b = palloc0(sizeof(RunReader));
	RunWriter  *w = palloc(sizeof(RunWriter));
	const JrEntry *x,
			   *y;
	JrRun		merged;

	a->run = older;
	b->run = newer;
	run_writer_start(w);
	x = run_reader_next(a);
	y = run_reader_next(b);
	while (x || y)
	{
		JrEntry		take;
		int			c = !x ? 1 : !y ? -1 : memcmp(x->hash, y->hash, DOLT_CHUNK_HASH_SIZE);

		/* Copy before advancing: a reader's next page replaces the one its entries point into. */
		if (c < 0)
		{
			take = *x;
			x = run_reader_next(a);
		}
		else
		{
			take = *y;
			y = run_reader_next(b);
			if (c == 0)
				x = run_reader_next(a);
		}
		if (oldest && (take.position & JR_TOMBSTONE_BIT))
			continue;
		run_writer_add(w, &take);
	}
	merged = run_writer_finish(w);
	pfree(a);
	pfree(b);
	pfree(w);
	return merged;
}

/* Publish |runs| as the run list. Under append_lock. */
static void
runs_publish(const JrRun *runs, uint32 nruns, bool clear_tail)
{
	LWLockAcquire(&shared->index_lock, LW_EXCLUSIVE);
	memcpy(shared->runs, runs, nruns * sizeof(JrRun));
	shared->nruns = nruns;
	if (clear_tail)
	{
		memset(shared->tail, 0, (Size) shared->capacity * sizeof(JrEntry));
		shared->tail_count = 0;
		shared->indexed = shared->end;
	}
	LWLockRelease(&shared->index_lock);
}

/*
 * Write the recent index as the newest run, merge runs as a binary counter
 * carries, and move the metapage. Under append_lock.
 */
static void
tail_flush(void)
{
	JrEntry    *entries = palloc_extended(Max(shared->tail_count, 1) * sizeof(JrEntry), MCXT_ALLOC_HUGE);
	uint64		n = 0;
	RunWriter  *w = palloc(sizeof(RunWriter));
	JrRun		runs[JR_MAX_RUNS + 1];
	uint32		nruns = shared->nruns;

	for (uint32 i = 0; i < shared->capacity; i++)
		if (shared->tail[i].position != 0)
			entries[n++] = shared->tail[i];
	qsort(entries, n, sizeof(JrEntry), compare_entries);
	run_writer_start(w);
	for (uint64 i = 0; i < n; i++)
		run_writer_add(w, &entries[i]);
	memcpy(runs, shared->runs, nruns * sizeof(JrRun));
	runs[nruns++] = run_writer_finish(w);
	pfree(entries);
	pfree(w);

	/* The stream the new run indexes must be durable before readers rely on the run. */
	stream_sync(shared->end);
	runs_publish(runs, nruns, true);

	while (nruns > 1 && (runs[nruns - 2].count <= 2 * runs[nruns - 1].count || nruns == JR_MAX_RUNS))
	{
		runs[nruns - 2] = runs_merge_pair(&runs[nruns - 2], &runs[nruns - 1], nruns == 2);
		nruns--;
		runs_publish(runs, nruns, false);
	}
	write_meta();
}

/* ---- Loading ---- */

static bool
read_meta(int slot, JrMeta *meta)
{
	PGAlignedBlock block;
	ssize_t		got = FileRead(index_file, block.data, BLCKSZ, (off_t) slot * BLCKSZ, WAIT_EVENT_DOLT_CHUNK_READ);

	if (got != BLCKSZ)
		return false;
	memcpy(meta, block.data, sizeof(JrMeta));
	return meta->magic == JR_MAGIC && meta->crc == meta_crc(meta) && meta->nruns <= JR_MAX_RUNS;
}

/*
 * Read the newer valid metapage, index the stream past it until a record
 * fails its CRC, and truncate the stream there. With |rebuild|, or if the
 * stream is shorter than the metapage says it covers, ignore the index and
 * index the whole stream: a base backup copies the two files at different
 * moments. Under append_lock, exclusive.
 */
static void
load(bool rebuild)
{
	JrMeta		metas[JR_META_SLOTS],
			   *meta = NULL;
	uint64		position,
				size;

	open_files();
	size = (uint64) FileSize(data_file);
	for (int i = 0; i < JR_META_SLOTS; i++)
		if (read_meta(i, &metas[i]) && (!meta || metas[i].generation > meta->generation))
			meta = &metas[i];
	if (meta && (rebuild || size < meta->indexed))
	{
		ereport(LOG, (errmsg("indexing the whole dolt chunk journal")));
		meta = NULL;
	}
	LWLockAcquire(&shared->index_lock, LW_EXCLUSIVE);
	memset(shared->tail, 0, (Size) shared->capacity * sizeof(JrEntry));
	shared->tail_count = 0;
	if (meta)
	{
		shared->generation = meta->generation;
		shared->indexed = meta->indexed;
		shared->next_free = meta->next_free;
		shared->nruns = meta->nruns;
		memcpy(shared->runs, meta->runs, sizeof(shared->runs));
	}
	else
	{
		shared->generation = 0;
		shared->indexed = 0;
		shared->next_free = JR_META_SLOTS;
		shared->nruns = 0;
	}
	LWLockRelease(&shared->index_lock);

	position = shared->indexed;
	shared->end = position;
	for (;;)
	{
		JrRecordHeader header;
		uint8	   *data = NULL;
		bool		valid;

		if (position + sizeof(header) > size)
			break;
		file_read(data_file, &header, sizeof(header), position, JR_DATA_PATH);
		if ((header.kind != JR_KIND_CHUNK && header.kind != JR_KIND_TOMBSTONE && header.kind != JR_KIND_UNLOGGED) ||
			position + sizeof(header) + header.length > size)
			break;
		if (header.length)
		{
			data = palloc_extended(header.length, MCXT_ALLOC_HUGE);
			file_read(data_file, data, header.length, position + sizeof(header), JR_DATA_PATH);
		}
		valid = record_crc(&header, data) == header.crc;
		if (data)
			pfree(data);
		if (!valid)
			break;
		LWLockAcquire(&shared->index_lock, LW_EXCLUSIVE);
		tail_set(header.hash, (position + 1) | (header.kind == JR_KIND_TOMBSTONE ? JR_TOMBSTONE_BIT : 0) |
				 (header.kind == JR_KIND_UNLOGGED ? JR_UNLOGGED_BIT : 0));
		LWLockRelease(&shared->index_lock);
		position += sizeof(header) + header.length;
		shared->end = position;
		if (tail_full())
			tail_flush();
	}
	if (position < size && FileTruncate(data_file, (off_t) position, WAIT_EVENT_DOLT_CHUNK_WRITE) != 0)
		ereport(FATAL, (errcode_for_file_access(), errmsg("could not truncate file \"%s\": %m", JR_DATA_PATH)));
	file_sync(data_file, JR_DATA_PATH);
	pg_atomic_write_u64(&shared->synced, position);
	if (!meta)
		write_meta();
	shared->loaded = true;
}

/* Load the journal if no process has: at startup, or in a backend of bootstrap or single-user mode. */
static void
ensure_loaded(void)
{
	open_files();
	if (shared->loaded)
		return;
	LWLockAcquire(&shared->append_lock, LW_EXCLUSIVE);
	if (!shared->loaded)
		load(false);
	LWLockRelease(&shared->append_lock);
}

/*
 * Called by the startup process before redo. A cluster restored from a base
 * backup indexes its whole journal anew.
 */
void
StartupDoltChunkJournal(bool from_backup)
{
	open_files();
	LWLockAcquire(&shared->append_lock, LW_EXCLUSIVE);
	if (!shared->loaded)
		load(from_backup);
	LWLockRelease(&shared->append_lock);
}

/*
 * Make every record appended so far durable, with the index: records whose
 * PUT precedes the checkpoint's redo point were appended before it.
 */
void
CheckPointDoltChunkJournal(void)
{
	uint64		end;

	if (!shared->loaded)
		return;
	open_files();
	LWLockAcquire(&shared->append_lock, LW_SHARED);
	end = shared->end;
	LWLockRelease(&shared->append_lock);
	stream_sync(end);
	file_sync(index_file, JR_INDEX_PATH);
}

/* ---- Lookups ---- */

/*
 * The located positions of |n| hashes: each record's position + 1 with the
 * tombstone bit, or 0.
 */
static void
locate_many(int n, const uint8 *hashes, uint64 *located)
{
	JrRun		runs[JR_MAX_RUNS];
	uint32		nruns;
	bool		any_missing = false;

	LWLockAcquire(&shared->index_lock, LW_SHARED);
	for (int i = 0; i < n; i++)
	{
		JrEntry    *entry = tail_find(hashes + (Size) i * DOLT_CHUNK_HASH_SIZE);

		located[i] = entry ? entry->position : 0;
		any_missing |= located[i] == 0;
	}
	nruns = shared->nruns;
	memcpy(runs, shared->runs, nruns * sizeof(JrRun));
	LWLockRelease(&shared->index_lock);
	if (!any_missing)
		return;

	/* Runs never change, so they are read without the lock. */
	for (int i = 0; i < n; i++)
		for (int r = (int) nruns - 1; located[i] == 0 && r >= 0; r--)
			located[i] = run_find(&runs[r], hashes + (Size) i * DOLT_CHUNK_HASH_SIZE);
}

static bool
live(uint64 located)
{
	return located != 0 && !(located & JR_TOMBSTONE_BIT);
}

/* ---- Appends ---- */

/*
 * Append records for chunks, or tombstones when |data| is NULL, and index
 * them; skip chunks the recent index already holds live, and logged if
 * |logged|. Returns how many it appended, marking them in |appended| if given.
 * Callers hold no lock.
 */
static int
append_records(int n, const uint8 *hashes, const uint8 *const *data, const Size *lengths, bool logged,
			   bool *appended)
{
	StringInfoData buf;
	uint64	   *positions = palloc(Max(n, 1) * sizeof(uint64));
	int			stored = 0;

	initStringInfo(&buf);
	LWLockAcquire(&shared->append_lock, LW_EXCLUSIVE);
	for (int i = 0; i < n; i++)
	{
		const uint8 *hash = hashes + (Size) i * DOLT_CHUNK_HASH_SIZE;
		JrRecordHeader header = {0};
		JrEntry    *entry;

		if (appended)
			appended[i] = false;
		positions[i] = 0;

		/* Only appenders change the recent index, and this one holds append_lock. */
		entry = tail_find(hash);
		/* A logged put of a chunk only an unlogged record holds appends it again, to log it. */
		if (data && entry && live(entry->position) && (!logged || !(entry->position & JR_UNLOGGED_BIT)))
			continue;
		if (!data && entry && !live(entry->position))
			continue;
		header.length = data ? (uint32) lengths[i] : 0;
		header.kind = !data ? JR_KIND_TOMBSTONE : logged ? JR_KIND_CHUNK : JR_KIND_UNLOGGED;
		memcpy(header.hash, hash, DOLT_CHUNK_HASH_SIZE);
		header.crc = record_crc(&header, data ? data[i] : NULL);
		positions[i] = shared->end + buf.len + 1;
		appendBinaryStringInfo(&buf, (const char *) &header, sizeof(header));
		if (header.length)
			appendBinaryStringInfo(&buf, (const char *) data[i], header.length);
		if (appended)
			appended[i] = true;
		stored++;
	}
	if (buf.len)
	{
		PG_TRY();
		{
			file_write(data_file, buf.data, buf.len, shared->end, JR_DATA_PATH);
		}
		PG_CATCH();
		{
			LWLockRelease(&shared->append_lock);
			PG_RE_THROW();
		}
		PG_END_TRY();
		shared->end += buf.len;
		LWLockAcquire(&shared->index_lock, LW_EXCLUSIVE);
		for (int i = 0; i < n; i++)
			if (positions[i])
				tail_set(hashes + (Size) i * DOLT_CHUNK_HASH_SIZE,
						 positions[i] | (data ? 0 : JR_TOMBSTONE_BIT) | (data && !logged ? JR_UNLOGGED_BIT : 0));
		LWLockRelease(&shared->index_lock);
		if (tail_full())
			tail_flush();
	}
	LWLockRelease(&shared->append_lock);
	pfree(buf.data);
	pfree(positions);
	return stored;
}

/* Log |n| chunks, or tombstones, in PUT records of at most JR_RECORD_BYTES of data each. */
static void
log_records(int n, const uint8 *hashes, const uint8 *const *data, const Size *lengths, const bool *appended)
{
	int			i = 0;

	while (i < n)
	{
		StringInfoData buf;
		xl_dolt_chunk_put header = {0};

		initStringInfo(&buf);
		appendBinaryStringInfo(&buf, (const char *) &header, SizeOfDoltChunkPut);
		for (; i < n && (header.count == 0 || buf.len < JR_RECORD_BYTES); i++)
		{
			xl_dolt_chunk_entry entry = {0};

			if (!appended[i])
				continue;
			entry.kind = data ? XLOG_DOLT_CHUNK_KIND_CHUNK : XLOG_DOLT_CHUNK_KIND_TOMBSTONE;
			entry.length = data ? (uint32) lengths[i] : 0;
			memcpy(entry.hash, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE, DOLT_CHUNK_HASH_SIZE);
			appendBinaryStringInfo(&buf, (const char *) &entry, SizeOfDoltChunkEntry);
			if (entry.length)
				appendBinaryStringInfo(&buf, (const char *) data[i], entry.length);
			header.count++;
		}
		if (header.count)
		{
			memcpy(buf.data, &header, SizeOfDoltChunkPut);
			XLogBeginInsert();
			XLogRegisterData(buf.data, buf.len);
			(void) XLogInsert(RM_DOLT_CHUNK_ID, XLOG_DOLT_CHUNK_PUT);
		}
		pfree(buf.data);
	}
}

/*
 * Append and, unless |logged| is false, log. A logged write takes a
 * transaction ID, so that its commit flushes the records; an unlogged one is
 * made durable before the transaction commits.
 */
static bool unlogged_pending;
static uint64 unlogged_end;

static int
write_records(int n, const uint8 *hashes, const uint8 *const *data, const Size *lengths, bool logged)
{
	bool	   *appended = palloc(Max(n, 1) * sizeof(bool));
	int			stored;

	ensure_loaded();
	if (logged && !RecoveryInProgress() && !IsBootstrapProcessingMode())
		(void) GetTopTransactionId();
	stored = append_records(n, hashes, data, lengths, logged, appended);
	if (stored && logged && !RecoveryInProgress())
		log_records(n, hashes, data, lengths, appended);
	else if (stored && !logged)
	{
		LWLockAcquire(&shared->append_lock, LW_SHARED);
		unlogged_end = Max(unlogged_end, shared->end);
		LWLockRelease(&shared->append_lock);
		unlogged_pending = true;
	}
	pfree(appended);
	return stored;
}

/* Make this transaction's unlogged appends durable before its commit record. */
void
dolt_chunk_journal_pre_commit(void)
{
	if (!unlogged_pending)
		return;
	stream_sync(unlogged_end);
	unlogged_pending = false;
	unlogged_end = 0;
}

/* At transaction end, an aborted transaction's unlogged appends need no sync. */
void
dolt_chunk_journal_xact_end(void)
{
	unlogged_pending = false;
	unlogged_end = 0;
}

/* ---- Redo ---- */

void
dolt_chunk_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	char	   *p = XLogRecGetData(record);
	char	   *end = p + XLogRecGetDataLen(record);
	xl_dolt_chunk_put header;
	uint8	   *hashes;
	const uint8 **data;
	Size	   *lengths;
	uint64	   *located;
	int			nchunks = 0,
				ntombs = 0;
	uint8	   *tomb_hashes;

	if (info != XLOG_DOLT_CHUNK_PUT)
		elog(PANIC, "dolt_chunk_redo: unknown op code %u", info);
	ensure_loaded();
	memcpy(&header, p, SizeOfDoltChunkPut);
	p += SizeOfDoltChunkPut;
	hashes = palloc(Max(header.count, 1) * DOLT_CHUNK_HASH_SIZE);
	tomb_hashes = palloc(Max(header.count, 1) * DOLT_CHUNK_HASH_SIZE);
	data = palloc(Max(header.count, 1) * sizeof(uint8 *));
	lengths = palloc(Max(header.count, 1) * sizeof(Size));
	for (uint32 i = 0; i < header.count; i++)
	{
		xl_dolt_chunk_entry entry;

		if (p + SizeOfDoltChunkEntry > end)
			elog(PANIC, "dolt_chunk_redo: malformed record");
		memcpy(&entry, p, SizeOfDoltChunkEntry);
		p += SizeOfDoltChunkEntry;
		if (p + entry.length > end)
			elog(PANIC, "dolt_chunk_redo: malformed record");
		if (entry.kind == XLOG_DOLT_CHUNK_KIND_TOMBSTONE)
			memcpy(tomb_hashes + (Size) ntombs++ * DOLT_CHUNK_HASH_SIZE, entry.hash, DOLT_CHUNK_HASH_SIZE);
		else
		{
			memcpy(hashes + (Size) nchunks * DOLT_CHUNK_HASH_SIZE, entry.hash, DOLT_CHUNK_HASH_SIZE);
			data[nchunks] = (const uint8 *) p;
			lengths[nchunks++] = entry.length;
		}
		p += entry.length;
	}
	if (p != end)
		elog(PANIC, "dolt_chunk_redo: malformed record");

	/* Append only what the journal lacks: the records may have reached it before a crash. */
	located = palloc(Max(nchunks, 1) * sizeof(uint64));
	locate_many(nchunks, hashes, located);
	{
		int			k = 0;

		for (int i = 0; i < nchunks; i++)
		{
			if (live(located[i]))
				continue;
			memmove(hashes + (Size) k * DOLT_CHUNK_HASH_SIZE, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE,
					DOLT_CHUNK_HASH_SIZE);
			data[k] = data[i];
			lengths[k++] = lengths[i];
		}
		if (k)
			(void) write_records(k, hashes, data, lengths, true);
	}
	if (ntombs)
		(void) write_records(ntombs, tomb_hashes, NULL, NULL, true);
	pfree(located);
	pfree(hashes);
	pfree(tomb_hashes);
	pfree(data);
	pfree(lengths);
}

/* ---- The routine ---- */

static Size
journal_max_size(void)
{
	return JR_MAX_CHUNK;
}

static void *
journal_open(DoltChunkIntent intent)
{
	JrState    *state = palloc0(sizeof(JrState));

	ensure_loaded();
	state->intent = intent;
	return state;
}

static void
journal_close(void *arg)
{
	pfree(arg);
}

static void
journal_get(void *arg, int n, const uint8 *hashes, DoltChunkFoundFn found, void *found_arg)
{
	uint64	   *located = palloc(Max(n, 1) * sizeof(uint64));

	locate_many(n, hashes, located);

	/* Records never move or change, so they are read without a lock. */
	for (int i = 0; i < n; i++)
	{
		JrRecordHeader header;
		uint8	   *data;

		if (!live(located[i]))
			continue;
		file_read(data_file, &header, sizeof(header), JR_POSITION(located[i]), JR_DATA_PATH);
		data = palloc_extended(Max(header.length, 1), MCXT_ALLOC_HUGE);
		file_read(data_file, data, header.length, JR_POSITION(located[i]) + sizeof(header), JR_DATA_PATH);
		found(found_arg, i, data, header.length);
		pfree(data);
	}
	pfree(located);
}

static void
journal_exists(void *arg, int n, const uint8 *hashes, bool *found)
{
	uint64	   *located = palloc(Max(n, 1) * sizeof(uint64));

	locate_many(n, hashes, located);
	for (int i = 0; i < n; i++)
		found[i] = live(located[i]);
	pfree(located);
}

static int
journal_put(void *arg, int n, const uint8 *hashes, const uint8 *const *data, const Size *lengths, bool logged)
{
	return write_records(n, hashes, data, lengths, logged);
}

static int
journal_remove(void *arg, int n, const uint8 *hashes)
{
	uint64	   *located = palloc(Max(n, 1) * sizeof(uint64));
	uint8	   *present = palloc(Max(n, 1) * DOLT_CHUNK_HASH_SIZE);
	int			k = 0,
				removed;

	locate_many(n, hashes, located);
	for (int i = 0; i < n; i++)
		if (live(located[i]))
			memcpy(present + (Size) k++ * DOLT_CHUNK_HASH_SIZE, hashes + (Size) i * DOLT_CHUNK_HASH_SIZE,
				   DOLT_CHUNK_HASH_SIZE);
	removed = k ? write_records(k, present, NULL, NULL, true) : 0;
	pfree(located);
	pfree(present);
	return removed;
}

/* Every live chunk record in the stream, in stream order, each hash once. */
static void
journal_scan(void *arg, DoltChunkScanFn fn, void *fn_arg)
{
	uint64		position = 0,
				end;

	LWLockAcquire(&shared->append_lock, LW_SHARED);
	end = shared->end;
	LWLockRelease(&shared->append_lock);
	while (position < end)
	{
		JrRecordHeader header;
		uint64		located;

		file_read(data_file, &header, sizeof(header), position, JR_DATA_PATH);
		if (header.kind == JR_KIND_CHUNK || header.kind == JR_KIND_UNLOGGED)
		{
			locate_many(1, header.hash, &located);

			/* Only the record the index names counts, so duplicates are seen once. */
			if (live(located) && JR_POSITION(located) == position && !fn(fn_arg, header.hash, header.length))
				break;
		}
		position += sizeof(header) + header.length;
	}
}

static void
journal_stats(DoltChunkStoreStats *stats)
{
	ensure_loaded();
	LWLockAcquire(&shared->index_lock, LW_SHARED);
	stats->chunks = shared->tail_count;
	for (uint32 r = 0; r < shared->nruns; r++)
		stats->chunks += shared->runs[r].count;
	LWLockRelease(&shared->index_lock);
	stats->bytes = (uint64) FileSize(data_file) + (uint64) FileSize(index_file);
}

static const DoltChunkStoreRoutine journal_routine = {
	.name = "journal",
	.max_size = journal_max_size,
	.open = journal_open,
	.close = journal_close,
	.get = journal_get,
	.exists = journal_exists,
	.put = journal_put,
	.scan = journal_scan,
	.remove = journal_remove,
	.stats = journal_stats,
};

const DoltChunkStoreRoutine *
dolt_chunk_journal_routine(void)
{
	return &journal_routine;
}
