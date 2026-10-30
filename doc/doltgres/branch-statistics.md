# Branch-local ANALYZE metadata

ANALYZE stores column distributions in PostgreSQL's `pg_statistic`, under the
branch relation's durable OID. Canonical relations have no writable `pg_class`
row; `pg_dolt_relation_stats` supplies their analyzed row total (`stattuples`,
double precision) and collection time (`statanalyzed`, timestamptz), keyed by
`statrelid`. This is native heap/WAL metadata, not part of any canonical object,
root, or commit. Historical runtime handles do not allocate metadata rows.

The `dolt.statistics` / `dolt_statistics` views combine that total with native
column statistics. The existing adapter reports one approximate bucket per
index, using the leading column's distribution; it does not reproduce Dolt's
prolly-node histogram partitioning or multi-column distinct-value estimates.
Ordinary physical relations continue to use `pg_class` statistics.

ANALYZE's relation lock serializes collection for one branch relation. Updates
and deletions are transactional. Schema invalidation, relation removal and
branch deletion remove the metadata with the corresponding `pg_statistic`
entries. A copied branch starts unanalyzed. Counts persist across restart but
remain unchanged after ordinary DML until the next ANALYZE. Autovacuum uses the
same collection path. The planner's table AM estimate uses the exact visible
row count, including small tables, without the former 1,000-row floor.
