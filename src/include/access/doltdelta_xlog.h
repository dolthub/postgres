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
#define XLOG_DOLT_DELTA_FOLD	0x10
#define XLOG_DOLT_DELTA_SETTLE	0x20

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

/*
 * A fold wrote a tree's base |root| holding the versions |xmin|, |xmax| and the
 * running xids that follow say, over the base |base| that held those its own
 * horizon says (none if |base_xmax| is invalid). A standby that lacks |root|
 * builds it from |base| and the versions it replayed, and prunes them once the
 * fold has committed and its own snapshots read past it.
 */
typedef struct xl_dolt_delta_fold
{
	Oid			db;
	Oid			space;
	RelFileNumber file;
	TransactionId fold;
	TransactionId xmin;
	TransactionId xmax;
	uint32		xcnt;
	TransactionId base_xmin;
	TransactionId base_xmax;
	uint32		base_xcnt;
	uint8		root[20];
	uint8		base[20];
	uint16		branchlen;
	/* branch name, then xcnt running xids, then base_xcnt of the base's */
} xl_dolt_delta_fold;

#define SizeOfDoltDeltaFold (offsetof(xl_dolt_delta_fold, branchlen) + sizeof(uint16))

/*
 * Keys of one tree settled without a fold: each key's versions, which every
 * snapshot sees and no base holds, replaced by one version with their net
 * count, or by none if it is zero. The versions are named by the WAL position
 * of their record and their xid; the first is kept, with the net count, the
 * record's position and |row|. A standby resolves conflicts with snapshots
 * that may not see |conflict|.
 */
typedef struct xl_dolt_delta_settle
{
	Oid			db;
	Oid			space;
	RelFileNumber file;
	TransactionId conflict;		/* the newest writer settled */
	uint32		count;			/* keys that follow */
	uint16		branchlen;
	/* branch name, then each key: xl_dolt_delta_settled, key, versions, row */
} xl_dolt_delta_settle;

#define SizeOfDoltDeltaSettle (offsetof(xl_dolt_delta_settle, branchlen) + sizeof(uint16))

typedef struct xl_dolt_delta_settled
{
	int64		delta;			/* the net count; zero drops the key */
	uint32		keylen;
	uint32		nversions;		/* xl_dolt_delta_version_id after the key */
	uint32		rowlen;			/* row bytes after them, when delta is not zero */
} xl_dolt_delta_settled;

#define SizeOfDoltDeltaSettled (offsetof(xl_dolt_delta_settled, rowlen) + sizeof(uint32))

typedef struct xl_dolt_delta_version_id
{
	XLogRecPtr	lsn;
	TransactionId topxid;
	TransactionId xid;
} xl_dolt_delta_version_id;

extern void dolt_delta_redo(XLogReaderState *record);
extern void dolt_delta_desc(StringInfo buf, XLogReaderState *record);
extern const char *dolt_delta_identify(uint8 info);

#endif							/* DOLTDELTA_XLOG_H */
