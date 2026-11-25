/*
 * Copyright 2026 Dolthub, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The read of the WAL kept for a manifest's fallback (design item 6): the
 * records between two positions, read from pg_wal by Postgres' WAL reader on
 * the timeline and up to the position its caller gives, with a WARNING naming
 * the segment that can't be read; a standby's read of the fork's records
 * past a storage's reached fold point (PLAN item 8's "Standbys' roots"); and
 * recovery's read of transactions' outcomes ahead of replay. The
 * reader's page callback is a function Postgres calls, and the loops run under
 * PG_TRY, so they are C.
 */

#include "postgres.h"

#include "access/xact.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "doltgres/kept_wal.h"
#include "replication/walreceiver.h"

/*
 * KeptRead is where a read of the kept WAL stands, as its caller chose: the
 * server's timeline, and the position its WAL can be read up to.
 */
typedef struct KeptRead
{
	TimeLineID	timeline;
	XLogRecPtr	upto;
} KeptRead;

/*
 * kept_page_read is the reader's page_read: read_local_xlog_page_no_wait's
 * read of a page, on the timeline and up to the position the read stands at
 * in place of the ones it asks the server for, which the server can't answer
 * before replay begins.
 */
static int
kept_page_read(XLogReaderState *state, XLogRecPtr targetPagePtr, int reqLen,
			   XLogRecPtr targetRecPtr, char *cur_page)
{
	KeptRead   *kept = state->private_data;
	XLogRecPtr	upto;
	int			count;
	WALReadError errinfo;

	XLogReadDetermineTimeline(state, targetPagePtr, reqLen, kept->timeline);
	upto = state->currTLI == kept->timeline ? kept->upto : state->currTLIValidUntil;
	if (targetPagePtr + XLOG_BLCKSZ <= upto)
		count = XLOG_BLCKSZ;
	else if (targetPagePtr + reqLen > upto)
		return -1;
	else
		count = upto - targetPagePtr;
	if (!WALRead(state, cur_page, targetPagePtr, count, state->currTLI, &errinfo))
		WALReadRaiseError(&errinfo);
	return count;
}

/*
 * record_start returns where the record after a position starts: the position
 * itself, or past the page's header when the record before it ended on a page
 * boundary.
 */
static XLogRecPtr
record_start(XLogRecPtr at)
{
	if (at % XLOG_BLCKSZ != 0)
		return at;
	return at + (XLogSegmentOffset(at, wal_segment_size) == 0 ? SizeOfXLogLongPHD : SizeOfXLogShortPHD);
}

/*
 * doltgres_warn_kept_wal reads the records that start between two positions,
 * on a timeline and up to a position, and reports at WARNING, naming the
 * segment, where it can't read one; an error of a read is caught, flushed, and
 * reported the same way, and the reader is freed either way.
 */
void
doltgres_warn_kept_wal(XLogRecPtr from, XLogRecPtr to, TimeLineID timeline, XLogRecPtr upto)
{
	KeptRead	kept = {.timeline = timeline, .upto = upto};
	MemoryContext caller = CurrentMemoryContext;
	XLogReaderState *reader;
	XLogRecPtr	at;
	char	   *message = NULL;

	reader = XLogReaderAllocate(wal_segment_size, NULL,
								XL_ROUTINE(.page_read = &kept_page_read,
										   .segment_open = &wal_segment_open,
										   .segment_close = &wal_segment_close),
								&kept);
	if (reader == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));
	PG_TRY();
	{
		char	   *errormsg = NULL;

		XLogBeginRead(reader, from);
		while (record_start(reader->NextRecPtr) < to)
		{
			if (XLogReadRecord(reader, &errormsg) == NULL)
			{
				message = errormsg != NULL ? pstrdup(errormsg) :
					psprintf("the WAL ends at %X/%08X", LSN_FORMAT_ARGS(reader->NextRecPtr));
				break;
			}
		}
	}
	PG_CATCH();
	{
		ErrorData  *error;

		MemoryContextSwitchTo(caller);
		error = CopyErrorData();
		FlushErrorState();
		message = error->message;
	}
	PG_END_TRY();
	at = reader->NextRecPtr;
	if (reader->currTLI != 0)
		timeline = reader->currTLI;
	XLogReaderFree(reader);
	if (message != NULL)
	{
		char		name[MAXFNAMELEN];
		XLogSegNo	segno;

		XLByteToSeg(at, segno, wal_segment_size);
		XLogFileName(name, timeline, segno, wal_segment_size);
		ereport(WARNING,
				(errmsg("could not read the WAL kept for a manifest's fallback in segment \"%s\": %s",
						name, message)));
	}
}

/*
 * doltgres_next_fork_record reads the first record of the fork's two resource
 * managers whose kind, its info's high four bits, 15 more under the second, is
 * in a mask, starting at a record's end and
 * up to the replayed WAL in recovery, or the received WAL when asked and it is
 * further, a record the WAL receiver has received only part of ending the
 * read, or otherwise the WAL to its last inserted record's end, which it
 * flushes first, since the flushed position may fall inside a record. It
 * writes the record's kind, its end, its data's length, and its data into the
 * buffer when they fit, and returns the bytes they need, or 0 when no such
 * record starts before the position; the reader is freed whatever its read
 * raises.
 */
uint64
doltgres_next_fork_record(XLogRecPtr from, RmgrId rmid, RmgrId second, uint32 kinds, bool received, char *data,
						  size_t len)
{
	KeptRead	kept = {0};
	XLogReaderState *reader;
	uint64		needed = 0;

	if (RecoveryInProgress())
	{
		kept.upto = GetXLogReplayRecPtr(&kept.timeline);
		if (received)
			kept.upto = Max(kept.upto, GetWalRcvFlushRecPtr(NULL, NULL));
	}
	else
	{
		kept.upto = GetXLogInsertEndRecPtr();
		XLogFlush(kept.upto);
		(void) GetFlushRecPtr(&kept.timeline);
	}
	reader = XLogReaderAllocate(wal_segment_size, NULL,
								XL_ROUTINE(.page_read = &kept_page_read,
										   .segment_open = &wal_segment_open,
										   .segment_close = &wal_segment_close),
								&kept);
	if (reader == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));
	PG_TRY();
	{
		char	   *errormsg = NULL;

		XLogBeginRead(reader, record_start(from));
		while (record_start(reader->NextRecPtr) < kept.upto)
		{
			uint8		kind;
			uint64		length;
			XLogRecord *read = XLogReadRecord(reader, &errormsg);

			if (read == NULL && received && errormsg == NULL)
				break;
			if (read == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("could not read the WAL at %X/%08X: %s",
								LSN_FORMAT_ARGS(reader->NextRecPtr),
								errormsg != NULL ? errormsg : "the WAL ends there")));
			if (XLogRecGetRmid(reader) != rmid && XLogRecGetRmid(reader) != second)
				continue;
			kind = ((XLogRecGetInfo(reader) & ~XLR_INFO_MASK) >> 4) + (XLogRecGetRmid(reader) == second ? 15 : 0);
			if ((kinds & (1U << kind)) == 0)
				continue;
			length = XLogRecGetDataLen(reader);
			needed = 1 + 2 * sizeof(uint64) + length;
			if (len >= needed)
			{
				data[0] = (char) kind;
				memcpy(data + 1, &reader->EndRecPtr, sizeof(uint64));
				memcpy(data + 1 + sizeof(uint64), &length, sizeof(uint64));
				memcpy(data + 1 + 2 * sizeof(uint64), XLogRecGetData(reader), length);
			}
			break;
		}
	}
	PG_FINALLY();
	{
		XLogReaderFree(reader);
	}
	PG_END_TRY();
	return needed;
}

/*
 * wal_outcome writes one outcome into a buffer of pairs while it fits, a
 * transaction's xid and the end of the record that committed or aborted it,
 * bit 63 set for an abort, and counts it either way.
 */
static void
wal_outcome(uint64 *pairs, size_t cap, size_t *count, TransactionId xid, XLogRecPtr end)
{
	if (*count < cap)
	{
		pairs[2 * *count] = xid;
		pairs[2 * *count + 1] = end;
	}
	(*count)++;
}

/*
 * doltgres_wal_outcomes reads, in recovery, the transaction records that
 * start between two positions on replay's timeline, ahead of replay, the WAL
 * up to the second being on disk, and writes into a buffer of pairs while
 * they fit each outcome they hold: every xid a COMMIT or COMMIT PREPARED
 * record committed, its subtransactions' included, with the record's end, and
 * every xid an ABORT or ABORT PREPARED record aborted with the record's end,
 * bit 63 set (bugs row 401).
 * It returns how many outcomes it found, and frees the reader whatever its
 * read raises.
 */
size_t
doltgres_wal_outcomes(XLogRecPtr from, XLogRecPtr to, uint64 *pairs, size_t cap)
{
	KeptRead	kept = {.upto = to};
	XLogReaderState *reader;
	size_t		count = 0;

	(void) GetXLogReplayRecPtr(&kept.timeline);
	reader = XLogReaderAllocate(wal_segment_size, NULL,
								XL_ROUTINE(.page_read = &kept_page_read,
										   .segment_open = &wal_segment_open,
										   .segment_close = &wal_segment_close),
								&kept);
	if (reader == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory"),
				 errdetail("Failed while allocating a WAL reading processor.")));
	PG_TRY();
	{
		char	   *errormsg = NULL;

		XLogBeginRead(reader, record_start(from));
		while (record_start(reader->NextRecPtr) < to)
		{
			uint8		info;

			if (XLogReadRecord(reader, &errormsg) == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("could not read the WAL at %X/%08X: %s",
								LSN_FORMAT_ARGS(reader->NextRecPtr),
								errormsg != NULL ? errormsg : "the WAL ends there")));
			if (XLogRecGetRmid(reader) != RM_XACT_ID)
				continue;
			info = XLogRecGetInfo(reader) & XLOG_XACT_OPMASK;
			if (info == XLOG_XACT_COMMIT || info == XLOG_XACT_COMMIT_PREPARED)
			{
				xl_xact_parsed_commit parsed;

				ParseCommitRecord(XLogRecGetInfo(reader), (xl_xact_commit *) XLogRecGetData(reader), &parsed);
				wal_outcome(pairs, cap, &count,
							info == XLOG_XACT_COMMIT_PREPARED ? parsed.twophase_xid : XLogRecGetXid(reader),
							reader->EndRecPtr);
				for (int i = 0; i < parsed.nsubxacts; i++)
					wal_outcome(pairs, cap, &count, parsed.subxacts[i], reader->EndRecPtr);
			}
			else if (info == XLOG_XACT_ABORT || info == XLOG_XACT_ABORT_PREPARED)
			{
				xl_xact_parsed_abort parsed;

				ParseAbortRecord(XLogRecGetInfo(reader), (xl_xact_abort *) XLogRecGetData(reader), &parsed);
				wal_outcome(pairs, cap, &count,
							info == XLOG_XACT_ABORT_PREPARED ? parsed.twophase_xid : XLogRecGetXid(reader),
							reader->EndRecPtr | UINT64CONST(0x8000000000000000));
				for (int i = 0; i < parsed.nsubxacts; i++)
					wal_outcome(pairs, cap, &count, parsed.subxacts[i],
								reader->EndRecPtr | UINT64CONST(0x8000000000000000));
			}
		}
	}
	PG_FINALLY();
	{
		XLogReaderFree(reader);
	}
	PG_END_TRY();
	return count;
}
