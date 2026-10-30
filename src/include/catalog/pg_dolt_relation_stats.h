/* Local ANALYZE metadata for durable branch relations, never versioned. */
#ifndef PG_DOLT_RELATION_STATS_H
#define PG_DOLT_RELATION_STATS_H

#include "catalog/genbki.h"
#include "catalog/pg_dolt_relation_stats_d.h"
#include "datatype/timestamp.h"

CATALOG(pg_dolt_relation_stats,9650,DoltRelationStatsRelationId)
{
	Oid			statrelid BKI_FORCE_NOT_NULL;
	float8		stattuples BKI_FORCE_NOT_NULL;
#ifdef CATALOG_VARLEN
	timestamptz statanalyzed BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_relation_stats;

typedef FormData_pg_dolt_relation_stats * Form_pg_dolt_relation_stats;

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_relation_stats_relid_index, 9651, DoltRelationStatsRelidIndexId, pg_dolt_relation_stats, btree(statrelid oid_ops));

#endif
