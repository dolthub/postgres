/*-------------------------------------------------------------------------
 *
 * doltdelta_xlog.h
 *	  WAL records of the prolly delta store.
 *
 * A record appends versions to one prolly tree's delta: the tree, the
 * writing transaction's top-level xid and command, and for each version its
 * key, a signed occurrence count and the row its key names. The record's own
 * xid is the writing (sub)transaction's.
 *
 * src/include/access/doltdelta_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DOLTDELTA_XLOG_H
#define DOLTDELTA_XLOG_H

#include "access/xlogreader.h"
#include "lib/stringinfo.h"
#include "storage/relfilelocator.h"

#define XLOG_DOLT_DELTA_APPEND	0x00

/* The branch name's room, as RootKey keeps it. */
#define DOLT_DELTA_BRANCH_SIZE	(NAMEDATALEN * 2)

typedef struct xl_dolt_delta_append
{
	Oid			db;
	Oid			space;
	RelFileNumber file;
	TransactionId topxid;
	CommandId	cid;
	uint32		count;			/* versions that follow */
	uint16		branchlen;		/* branch name bytes that follow, before them */
	/* branch name, then each version: xl_dolt_delta_version, key, row */
} xl_dolt_delta_append;

#define SizeOfDoltDeltaAppend (offsetof(xl_dolt_delta_append, branchlen) + sizeof(uint16))

typedef struct xl_dolt_delta_version
{
	int64		delta;
	uint32		keylen;
	uint32		rowlen;
	/* key bytes, then row bytes */
} xl_dolt_delta_version;

#define SizeOfDoltDeltaVersion (offsetof(xl_dolt_delta_version, rowlen) + sizeof(uint32))

extern void dolt_delta_redo(XLogReaderState *record);
extern void dolt_delta_desc(StringInfo buf, XLogReaderState *record);
extern const char *dolt_delta_identify(uint8 info);

#endif							/* DOLTDELTA_XLOG_H */
