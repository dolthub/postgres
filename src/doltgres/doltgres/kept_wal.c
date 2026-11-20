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
 * the segment that can't be read; and a standby's read of the fork's records
 * past a storage's reached fold point (PLAN item 8's "Standbys' roots"). The
 * reader's page callback is a function Postgres calls, and the loops run under
 * PG_TRY, so they are C.
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogreader.h"
#include "access/xlogrecovery.h"
#include "access/xlogutils.h"
#include "doltgres/kept_wal.h"

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
 * doltgres_next_fork_record reads the first record of a resource manager whose
 * kind, its info's high four bits, is in a mask, starting at a record's end and
 * up to the replayed WAL in recovery or the flushed WAL otherwise. It writes the
 * record's kind, its end, its data's length, and its data into the buffer when
 * they fit, and returns the bytes they need, or 0 when no such record starts
 * before the position; the reader is freed whatever its read raises.
 */
uint64
doltgres_next_fork_record(XLogRecPtr from, RmgrId rmid, uint32 kinds, char *data, size_t len)
{
	KeptRead	kept = {0};
	XLogReaderState *reader;
	uint64		needed = 0;

	if (RecoveryInProgress())
		kept.upto = GetXLogReplayRecPtr(&kept.timeline);
	else
		kept.upto = GetFlushRecPtr(&kept.timeline);
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

			if (XLogReadRecord(reader, &errormsg) == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("could not read the WAL at %X/%08X: %s",
								LSN_FORMAT_ARGS(reader->NextRecPtr),
								errormsg != NULL ? errormsg : "the WAL ends there")));
			kind = (XLogRecGetInfo(reader) & ~XLR_INFO_MASK) >> 4;
			if (XLogRecGetRmid(reader) != rmid || (kinds & (1U << kind)) == 0)
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
