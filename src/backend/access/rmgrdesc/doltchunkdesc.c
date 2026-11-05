/*-------------------------------------------------------------------------
 *
 * doltchunkdesc.c
 *	  rmgr descriptor routines for the prolly chunk journal
 *
 * src/backend/access/rmgrdesc/doltchunkdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/doltchunk_xlog.h"

void
dolt_chunk_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	char	   *end = rec + XLogRecGetDataLen(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_DOLT_CHUNK_PUT)
	{
		xl_dolt_chunk_put header;
		uint32		chunks = 0,
					tombstones = 0;
		uint64		bytes = 0;

		memcpy(&header, rec, SizeOfDoltChunkPut);
		rec += SizeOfDoltChunkPut;
		for (uint32 i = 0; i < header.count && rec + SizeOfDoltChunkEntry <= end; i++)
		{
			xl_dolt_chunk_entry entry;

			memcpy(&entry, rec, SizeOfDoltChunkEntry);
			rec += SizeOfDoltChunkEntry + entry.length;
			if (entry.kind == XLOG_DOLT_CHUNK_KIND_TOMBSTONE)
				tombstones++;
			else
			{
				chunks++;
				bytes += entry.length;
			}
		}
		appendStringInfo(buf, "%u chunks of %llu bytes; %u tombstones", chunks, (unsigned long long) bytes,
						 tombstones);
	}
}

const char *
dolt_chunk_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_DOLT_CHUNK_PUT:
			return "PUT";
	}
	return NULL;
}
