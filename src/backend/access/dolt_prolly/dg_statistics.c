/* ANALYZE's row total belongs to a branch relation, like pg_statistic rows.
 * There is no writable pg_class row for a canonical relation. Keep this
 * local metadata separately; it must not affect canonical content hashes.
 */
#include "postgres.h"
#include "access/doltcatalog.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/pg_dolt_relation_stats.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

/* ANALYZE holds ShareUpdateExclusiveLock on the durable relation. */
void
dg_store_relation_stats(Oid relid, double rows)
{
	Relation	catalog = table_open(DoltRelationStatsRelationId, RowExclusiveLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	old,
				tuple;
	Datum		values[Natts_pg_dolt_relation_stats];
	bool		nulls[Natts_pg_dolt_relation_stats] = {false};

	values[Anum_pg_dolt_relation_stats_statrelid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_dolt_relation_stats_stattuples - 1] = Float8GetDatum(rows);
	values[Anum_pg_dolt_relation_stats_statanalyzed - 1] = TimestampTzGetDatum(GetCurrentTimestamp());
	tuple = heap_form_tuple(RelationGetDescr(catalog), values, nulls);
	ScanKeyInit(&key, Anum_pg_dolt_relation_stats_statrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(catalog, DoltRelationStatsRelidIndexId, true, NULL, 1, &key);
	old = systable_getnext(scan);
	if (HeapTupleIsValid(old))
		CatalogTupleUpdate(catalog, &old->t_self, tuple);
	else
		CatalogTupleInsert(catalog, tuple);
	systable_endscan(scan);
	heap_freetuple(tuple);
	table_close(catalog, RowExclusiveLock);
}

/* Called with pg_statistic cleanup on schema change, DROP and branch deletion. */
void
dg_remove_relation_stats(Oid relid)
{
	Relation	catalog = table_open(DoltRelationStatsRelationId, RowExclusiveLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;

	ScanKeyInit(&key, Anum_pg_dolt_relation_stats_statrelid, BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(relid));
	scan = systable_beginscan(catalog, DoltRelationStatsRelidIndexId, true, NULL, 1, &key);
	tuple = systable_getnext(scan);
	if (HeapTupleIsValid(tuple))
		CatalogTupleDelete(catalog, &tuple->t_self);
	systable_endscan(scan);
	table_close(catalog, RowExclusiveLock);
}
