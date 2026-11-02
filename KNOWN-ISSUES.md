# Deferred defects

User-directed deferrals from func-review.txt (2026-10-23). These remain defects,
not supported semantics. Address independently with regression tests.

* **Prepared dynamic diff result layouts:** preparing dolt_diff, changing the
  table schema and executing the plan can crash the backend. Bind result schema
  dependencies and validate runtime descriptors before writing tuple arrays.
* **Analysis-only dolt_ignore creation:** EXPLAIN/PREPARE of the first INSERT can
  create the table during parsing. The new lazy docs/tests/query-catalog table
  adapters use the same mechanism. Move lazy creation to execution.
* **dolt_query_diff row equality:** unescaped text separators permit distinct
  rows to compare equal. Replace with typed length-delimited encoding.

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

* **Row journal disabled: unsafe with concurrent writers (2026-10-31):** small
  batches can be journaled to `pg_dolt_journal` instead of rewriting a table's
  trees, but `dolt_row_journal` is now off by default. A writer folds the
  journal into the latest state, then persists that fold mid-transaction:
  it saves roots that include other transactions' entries and deletes them.
  Its own later snapshot reads merge those roots in as if they were its own
  changes, and concurrent folders race to consume the same entries. Under
  concurrent TPC-C this lost updates (duplicate order numbers, money and stock
  created or lost), showed repeatable read transactions rows committed after
  their snapshot, and failed with "prolly journal removes a missing index
  entry" and "failed to fetch tuple being updated". Partial fixes are merged
  (fold only entries the snapshot sees, never refold consumed entries, reload a
  latest state whose roots moved), but other interleavings remain. Folding
  needs separating from writers, for example compaction by something whose
  snapshot sees everything it folds, before the journal can be on by default.
  Its single-threaded TPC-C gain was roughly 5-10% at scale 1.
  `test_prolly/t/050_concurrent_writes.pl` fails with the journal on and is
  the gate for re-enabling it, with the isolation specs in
  `test_prolly/specs`.

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
