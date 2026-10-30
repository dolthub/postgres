# Compatibility surface: statistics, backups, remotes, clusters and control tables

Reference: Dolt `go/libraries/doltcore/schema/statistic.go`,
`sqle/dtables/{statistics_table,backups_table,query_catalog_table,branch_control_table,
branch_namespace_control,branch_activity_table}.go`, `dprocedures/{dolt_stats,dolt_backup,
dolt_fetch,dolt_push,dolt_pull,dolt_clone,dolt_cluster}.go`; Doltgres
`testing/go/{stats_usage_test,dolt_backup_test,dolt_remote_test,pgcatalog_test}.go`.

These pieces give the Dolt names a PostgreSQL meaning where one exists and a
clear error where it does not. Native file backups now transfer canonical objects between forks.

## dolt.statistics and dolt_stats_*

`dolt.statistics` (`dolt_statistics`) is a view over `pg_class`, `pg_index`,
`pg_stats` and `pg_stat_all_tables` with Dolt's columns (`database_name,
table_name, index_name, row_count, distinct_count, null_count, columns, types,
upper_bound, upper_bound_cnt, created_at, mcv1..mcv4, mcv_counts`). Dolt stores
one row per histogram bucket; here each index of an analyzed table is one
bucket: `row_count` is `reltuples`, `distinct_count`/`null_count` come from the
first indexed column's `n_distinct`/`null_frac`, `upper_bound` is the last
histogram bound, and the most-common values map to `mcv*`. The primary key
index is named `primary`. Tables that were never analyzed have no rows.
`dolt_stats_once()`, `dolt_stats_wait()` and `dolt_stats_flush()` run `ANALYZE`
on every versioned user table; `dolt_stats_stop/restart/gc/purge/timers` are
no-ops that explain that autovacuum owns collection; `dolt_stats_info()`
returns a JSON summary over `pg_stats`.

The earlier ANALYZE limitation is resolved. See
[branch-local statistics](branch-statistics.md) for collection and current
single-bucket approximation limits.

## dolt_backup and dolt.backups

`dolt_backup('add', name, url)` and `dolt_backup('remove', name)` maintain
`pg_catalog.pg_dolt_backup`, listed by `dolt.backups`, `dolt.dolt_backups` and
`dolt_backups` (`name, url`). `sync`, `sync-url` and `restore` support native
file bundles; see [native backups](native-backups.md).

## dolt_fetch, dolt_push, dolt_pull, dolt_clone, cluster procedures

Defined so callers get a precise error instead of "function does not exist":
each raises `FEATURE_NOT_SUPPORTED` ("… is not supported: the server has no
remote transport for manifest objects"). Remote definitions still live in
`dolt.remotes`. `dolt_assume_cluster_role` and
`dolt_cluster_transition_to_standby` explain that replication is PostgreSQL
streaming replication.

## dolt.query_catalog, dolt.branch_control, dolt.branch_namespace_control

`dolt.query_catalog` and `dolt_query_catalog` access the ordinary versioned
`public.dolt_query_catalog` prolly table. Branch control uses native unversioned
catalog tables with updatable views and aliases. Branch control rows are stored
but not enforced: every version-control call still requires superuser in this
prototype.

## dolt.branch_activity

`branch, last_read (NULL), last_write (latest reflog entry of the branch),
active_sessions (0), system_start_time (postmaster start)`.

Tests: `src/test/modules/test_prolly/t/025_compat.pl`; Doltgres
`StatsAggregateTests`/`StatsUsageTests` scenarios where the fork's single-bucket
approximation satisfies them.
