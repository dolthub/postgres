# Deferred defects

User-directed deferrals from func-review.txt (2026-10-23). These remain defects,
not supported semantics. Address independently with regression tests.

* **Prepared dynamic diff result layouts:** preparing dolt_diff, changing the
  table schema and executing the plan can crash the backend. Bind result schema
  dependencies and validate runtime descriptors before writing tuple arrays.
* **Analysis-only dolt_ignore creation:** EXPLAIN/PREPARE of the first INSERT can
  create the table during parsing. The new lazy docs/tests/query-catalog table
  adapters use the same mechanism. Move lazy creation to execution.

The SQL reproductions are in /workspace/reports/rewrite-dolt-functions-*.sql.

* **Forced branch deletion and old sessions:** ordinary deletion rejects active
  sessions, and writes after a visible deletion fail. A transaction retaining an
  older snapshot, or a session surviving forced deletion and same-name recreation,
  still needs a commit-time authority check. This is the existing-session edge
  explicitly allowed to defer in func-review.txt. Branch creation itself now
  overwrites working refs and table roots, so it does not resurrect old contents.

* **Committed chunks stored twice under two hashes (2026-10-26):** a commit
  copies every node of every table tree from `pg_dolt_chunk` into
  `pg_dolt_object` as `chunk` objects keyed by a manifest envelope hash, not the
  node hash the tree itself uses. History reads import those copies one SPI
  statement per chunk. See [chunk store plan](CHUNK-STORE-PLAN.md) milestone 6;
  the analysis is in /workspace/reports/catalog-bind-analysis/index.html.

* **Schemas fingerprinted outside the hash namespace (2026-10-30):** a member
  descriptor (table, index, routine, view, type, trigger, sequence) and the
  database object's member list carry a `schema` field holding a hex SHA-256 of
  the definition text, computed in SQL at about eight call sites. It is a second
  hash of content the descriptor already names as a `schema` object child, and
  it is part of the hashed payloads, so it fixes descriptor, database and commit
  hashes. Diff, status and merge compare it to detect schema changes. A schema
  should be a chunk like any other: stored in the chunk store and referred to
  by the manifest by its chunk address, which those comparisons then use. Found
  while moving content hashes to 20 bytes on zachmu/short-hashes, which leaves
  these fingerprints at 32.

* **Row journal removed (2026-11-04):** the row journal (`pg_dolt_journal`,
  `dolt_row_journal`) lost updates under concurrent writers, because writers
  folded it into roots they saved. It is removed on `zachmu/doltgrust-deltas`,
  which replaces it with the delta store of
  [DELTA-STORE-PLAN.md](DELTA-STORE-PLAN.md): durable shared deltas over
  per-tree bases, folded only by background workers.
  `test_prolly/t/050_concurrent_writes.pl` and the isolation specs in
  `test_prolly/specs` remain the gate for concurrent writers.

* **Concurrent identical keyless rows share a derived handle (2026-11-02):** a
  keyless row's handle is derived from its digest unless the writer's view
  already holds an occurrence of that row, in which case it allocates one
  (`dg_handle_choose`). Two transactions that insert the same row, neither
  seeing the other's, both choose the derived handle. Publication then sums the
  content counts (`merge_counts`), but two occurrences share one handle and its
  lock record, and no allocation names the second. Seen only as a design gap:
  `test_prolly/t/027_row_locks.pl` checks that all occurrences stay counted and
  updatable afterwards, not that each keeps a distinct handle. The window was a
  statement before `dolt_snapshot_writes`; with it on, it is a whole repeatable
  read transaction. A fix could check the latest committed state, and other
  writers' reservations, before deriving, or reassign colliding handles when
  publication merges counts.

* **No spatial indexes on prolly tables (2026-10-30):** GiST, SP-GiST and BRIN
  are rejected on prolly tables, and PostGIS `geometry` and `geography` have no
  ordered key encoding for a prolly index. Spatial values are stored, versioned,
  diffed and merged, but every spatial predicate scans the whole table, which
  rules out real spatial workloads. Accepted for now. The long-term answer is a
  versioned spatial index built from prolly trees, for example over a
  space-filling-curve key (geohash or Hilbert cell) with an exact recheck, rather
  than a native GiST rebuilt per branch, which would not be history-independent.

* **A prepared transaction blocks every commit to its tables (2026-11-02):**
  PREPARE publishes like a commit: it merges onto the latest roots, writes the
  (table, branch) row in `pg_dolt_root` and keeps that row locked until COMMIT
  or ROLLBACK PREPARED. Any other transaction that writes the table on that
  branch, even different rows, waits at its own COMMIT or PREPARE. Readers and
  other branches are unaffected. Heap blocks only writers of the same rows or
  unique keys, so a forgotten prepared transaction here freezes writes to whole
  tables. Accepted for now. The recovery TAP files 009_twophase,
  021_row_visibility and 023_pitr_prepared_xact depend on concurrent prepared
  writers and are skipped in prolly-default runs. Deferring the merge to COMMIT
  PREPARED would fix the concurrency, but COMMIT PREPARED must not fail, so
  versioned DDL and branch operations (merge, reset, revert, cherry-pick) would
  have to wait for pending prepared writers, and the root write would have to
  commit atomically with the prepared transaction rather than with the session
  running COMMIT PREPARED. Pinned by `test_prolly/t/035_prepared_locks.pl`.
