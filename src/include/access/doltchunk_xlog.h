/*-------------------------------------------------------------------------
 *
 * doltchunk_xlog.h
 *	  WAL records of the prolly chunk journal.
 *
 * A PUT record carries chunks that cannot be derived from logged row edits,
 * or tombstones that delete chunks: each entry's kind, hash and length, then
 * its bytes. Redo appends to the journal whatever it lacks.
 *
 * src/include/access/doltchunk_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DOLTCHUNK_XLOG_H
#define DOLTCHUNK_XLOG_H

#include "access/doltchunk.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"

#define XLOG_DOLT_CHUNK_PUT		0x00

#define XLOG_DOLT_CHUNK_KIND_CHUNK		1
#define XLOG_DOLT_CHUNK_KIND_TOMBSTONE	2

typedef struct xl_dolt_chunk_put
{
	uint32		count;			/* entries that follow */
} xl_dolt_chunk_put;

#define SizeOfDoltChunkPut	(offsetof(xl_dolt_chunk_put, count) + sizeof(uint32))

typedef struct xl_dolt_chunk_entry
{
	uint32		length;			/* bytes that follow the entry */
	uint8		kind;
	uint8		hash[DOLT_CHUNK_HASH_SIZE];
} xl_dolt_chunk_entry;

#define SizeOfDoltChunkEntry	(offsetof(xl_dolt_chunk_entry, hash) + DOLT_CHUNK_HASH_SIZE)

extern void dolt_chunk_redo(XLogReaderState *record);
extern void dolt_chunk_desc(StringInfo buf, XLogReaderState *record);
extern const char *dolt_chunk_identify(uint8 info);

#endif							/* DOLTCHUNK_XLOG_H */
