/*-------------------------------------------------------------------------
 *
 * doltdeltadesc.c
 *	  rmgr descriptor routines for the prolly delta store
 *
 * src/backend/access/rmgrdesc/doltdeltadesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/doltdelta_xlog.h"

void
dolt_delta_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_DOLT_DELTA_APPEND)
	{
		xl_dolt_delta_append header;

		memcpy(&header, rec, SizeOfDoltDeltaAppend);
		appendStringInfo(buf, "tree %u/%u/%u branch %.*s; topxid %u cid %u; %u versions",
						 header.db, header.space, header.file, (int) header.branchlen,
						 rec + SizeOfDoltDeltaAppend, header.topxid, header.cid, header.count);
	}
	else if (info == XLOG_DOLT_DELTA_FOLD)
	{
		xl_dolt_delta_fold header;

		memcpy(&header, rec, SizeOfDoltDeltaFold);
		appendStringInfo(buf, "tree %u/%u/%u branch %.*s; fold %u holds xmin %u xmax %u with %u running",
						 header.db, header.space, header.file, (int) header.branchlen,
						 rec + SizeOfDoltDeltaFold, header.fold, header.xmin, header.xmax, header.xcnt);
	}
}

const char *
dolt_delta_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_DOLT_DELTA_APPEND:
			return "APPEND";
		case XLOG_DOLT_DELTA_FOLD:
			return "FOLD";
	}
	return NULL;
}
